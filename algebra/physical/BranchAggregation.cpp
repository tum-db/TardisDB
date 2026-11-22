#include "algebra/physical/BranchAggregation.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <numeric>

#include "codegen/LLVMCompat.hpp"

#include "foundations/Parallel.hpp"
#include "sql/SqlTuple.hpp"
#include "sql/SqlUtils.hpp"

using namespace Sql;

namespace Sql {
void storeTextGen(char *dest, const uint8_t * bytes, size_t len);
}

namespace Algebra {
namespace Physical {

using Function = Logical::BranchAggregation::Function;
using State = BranchAggregation::State;

//-----------------------------------------------------------------------------
// Runtime functions called by the compiled query

// what the slot holds: the function of its aggregate, or Count for the count of the tuples and the
// counts of the values that are not NULL, which are summed like counts
static Function slot_function(const State * state, size_t slot)
{
    return (slot >= 1 && slot <= state->functions.size()) ? static_cast<Function>(state->functions[slot - 1]) : Function::Count;
}

// the bits of doubles as slots hold them
static int64_t addDoubles(int64_t a, int64_t b)
{
    double x, y;
    std::memcpy(&x, &a, 8);
    std::memcpy(&y, &b, 8);
    x += y;
    std::memcpy(&a, &x, 8);
    return a;
}

static void reset_state(State * state)
{
    const size_t groups = state->set.branches.size() + 1;
    for (size_t worker = 0; worker < state->workers; ++worker)
    for (size_t g = 0; g < groups; ++g) {
        int64_t * slots = state->direct.data() + worker * state->groupStride + g * state->width;
        for (size_t slot = 0; slot < state->width; ++slot) {
            switch (slot_function(state, slot)) {
                case Function::Min: slots[slot] = std::numeric_limits<int64_t>::max(); break;
                case Function::Max: slots[slot] = std::numeric_limits<int64_t>::min(); break;
                default: slots[slot] = 0;
            }
        }
    }
    std::fill(state->diff.begin(), state->diff.end(), 0);
}

// merges the groups of the workers, then combines per reader its own group, the group read by all
// readers and the prefix sum of the difference array
static void finish_state(State * state)
{
    const size_t w = state->width;
    const size_t k = state->set.branches.size();
    const size_t perWorker = state->groupStride;
    int64_t * direct = state->direct.data();
    int64_t * diff = state->diff.data();
    for (size_t worker = 1; worker < state->workers; ++worker) {
        for (size_t g = 0; g <= k; ++g) {
            const int64_t * own = direct + worker * perWorker + g * w;
            int64_t * merged = direct + g * w;
            for (size_t slot = 0; slot < w; ++slot) {
                switch (slot_function(state, slot)) {
                    case Function::Min: merged[slot] = std::min(merged[slot], own[slot]); break;
                    case Function::Max: merged[slot] = std::max(merged[slot], own[slot]); break;
                    default:
                        if (state->doubleSums[slot]) merged[slot] = addDoubles(merged[slot], own[slot]);
                        else merged[slot] += own[slot];
                }
            }
        }
        for (size_t i = 0; i < (k + 1) * w; ++i) {
            diff[i] += diff[worker * perWorker + i];
        }
    }

    const int64_t * all = direct + k * w;
    std::vector<int64_t> prefix(w, 0);
    std::vector<int64_t> combined(w);
    std::vector<int64_t> byPosition(k * w);
    for (size_t p = 0; p < k; ++p) {
        const int64_t * own = direct + p * w;
        int64_t * out = byPosition.data() + p * w;
        // the slots of the reader: its own group, the group read by all, and the ranges up to it
        for (size_t slot = 0; slot < w; ++slot) {
            prefix[slot] += diff[p * w + slot];
            switch (slot_function(state, slot)) {
                case Function::Min: combined[slot] = std::min(own[slot], all[slot]); break;
                case Function::Max: combined[slot] = std::max(own[slot], all[slot]); break;
                default:
                    if (state->doubleSums[slot]) combined[slot] = addDoubles(addDoubles(own[slot], all[slot]), prefix[slot]);
                    else combined[slot] = own[slot] + all[slot] + prefix[slot];
            }
        }
        std::copy(combined.begin(), combined.end(), out);
        for (size_t i = 0; i < state->functions.size(); ++i) {
            const size_t slot = 1 + i;
            const int64_t values = combined[state->nonNull[i]]; // the tuples, or the values that are not NULL
            switch (static_cast<Function>(state->functions[i])) {
                case Function::Count: out[slot] = values; break;
                case Function::Sum: out[slot] = combined[slot]; break;
                case Function::Avg:
                    if (state->doubleSums[slot]) {
                        double sum;
                        std::memcpy(&sum, &combined[slot], 8);
                        sum = (values > 0) ? sum / static_cast<double>(values) : 0.0;
                        std::memcpy(&out[slot], &sum, 8);
                    } else {
                        out[slot] = (values > 0) ? combined[slot] / values : 0;
                    }
                    break;
                case Function::Min:
                case Function::Max: out[slot] = combined[slot]; break;
            }
        }
    }
    for (size_t j = 0; j < k; ++j) {
        std::copy_n(byPosition.data() + state->position[j] * w, w, state->result.data() + j * w);
    }
}

// in the PerBranch strategy, the workers scan one branch after the other
static void run_morsels(State * state, void * function, void * context)
{
#if !INTERVAL_VISIBILITY
    // the lineage maps are built after the query has been compiled
    auto & executionContext = static_cast<QueryContext *>(context)->executionContext;
    for (size_t p = 0; p < state->set.branches.size(); ++p) {
        auto it = executionContext.branch_lineages.find(state->set.branches[p]);
        state->lineages[p] = (it != executionContext.branch_lineages.end()) ? &it->second : nullptr;
    }
#endif
    Parallel::runMorsels(state->workers, state->tuples, state->perBranch ? state->set.branches.size() : 1,
                         reinterpret_cast<Parallel::morsel_function_t>(function));
}

static uint32_t visible_ranges(size_t tid, void * table, void * state, size_t allPresent, size_t worker)
{
    auto * s = static_cast<State *>(state);
    const size_t offset = worker * s->scratchStride;
    return get_visible_ranges(tid, *static_cast<Table *>(table), s->set, allPresent != 0,
                              s->ptrs.data() + offset, s->lo.data() + offset, s->hi.data() + offset);
}

static const void * latest_entry(size_t tid, void * table, void * state, size_t position)
{
    auto * s = static_cast<State *>(state);
#if INTERVAL_VISIBILITY
    return get_latest_entry_of_reader(tid, *static_cast<Table *>(table), s->set.branches[position], s->set.labels[position], nullptr);
#elif COMPILED_LINEAGE
    return get_latest_entry_of_reader(tid, *static_cast<Table *>(table), s->set.branches[position], 0, nullptr,
                                      s->set.limits.data() + position * s->set.stride);
#else
    return get_latest_entry_of_reader(tid, *static_cast<Table *>(table), s->set.branches[position], 0, s->lineages[position]);
#endif
}

static llvm::Function * declareFunction(const std::string & name, llvm::FunctionType * type, void * function)
{
    auto & moduleGen = getThreadLocalCodeGen().getCurrentModuleGen();
    llvm::Function * func = LLVMCompat::getOrInsertFunction(moduleGen.getModule(), name, type);
    moduleGen.addFunctionMapping(func, function);
    return func;
}

template<typename T>
static llvm::Value * rawPointer(T * ptr, llvm::Type * elementTy)
{
    auto & codeGen = getThreadLocalCodeGen();
    return codeGen->CreateIntToPtr(cg_size_t(reinterpret_cast<uintptr_t>(ptr)).getValue(), llvm::PointerType::getUnqual(elementTy));
}

//-----------------------------------------------------------------------------
// BranchAggregation

BranchAggregation::BranchAggregation(const logical_operator_t & logicalOperator, std::vector<Expressions::exp_op_t> predicates, QueryContext & queryContext) :
        NullaryOperator(logicalOperator, queryContext),
        _table(static_cast<const Logical::BranchAggregation &>(logicalOperator).getTable()),
        _predicates(std::move(predicates))
{
    auto & op = static_cast<const Logical::BranchAggregation &>(logicalOperator);
    if (_table.size() < 1) return; // nothing to scan

    // the columns read by predicates and aggregates
    iu_set_t needed;
    for (auto & predicate : op._predicates) {
        iu_set_t required = Logical::collectRequired(*predicate);
        needed.insert(required.begin(), required.end());
    }
    for (auto & aggregate : op.getAggregates()) {
        if (aggregate.input != nullptr) needed.insert(aggregate.input);
    }

    for (iu_p_t iu : needed) {
        ci_p_t ci = getColumnInformation(iu);
        size_t index = ci->index;
        llvm::Type * columnTy = llvm::ArrayType::get(toLLVMTy(ci->type), _table.size());
        llvm::Value * columnPtr = createPointerValue(ci->column->front(), columnTy);
        _columns.emplace_back(iu, ci, index, columnTy, columnPtr);
    }
}

BranchAggregation::~BranchAggregation()
{ }

void BranchAggregation::produce()
{
    auto & op = static_cast<const Logical::BranchAggregation &>(_logicalOperator);
    auto & context = _codeGen.getLLVMContext();
    Database & db = _table.getDatabase();
    const auto & branches = op.getBranches();
    const size_t k = branches.size();

    auto owned = std::make_unique<State>();
    State & state = *owned;
    _context.executionContext.acquireResource(std::move(owned));

    // readers ordered by label, so that the readers of a version form a range
    std::vector<uint32_t> order(k);
    std::iota(order.begin(), order.end(), 0);
#if INTERVAL_VISIBILITY
    auto label = [&](uint32_t j) { return db._identL[db.getIdent(branches[j])]; };
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return label(a) < label(b); });
#endif
    state.position.resize(k);
    for (uint32_t p = 0; p < k; ++p) {
        state.set.branches.push_back(branches[order[p]]);
#if INTERVAL_VISIBILITY
        state.set.labels.push_back(label(order[p]));
#endif
        state.position[order[p]] = p;
    }
    state.width = 1 + op.getAggregates().size();
    for (auto & aggregate : op.getAggregates()) {
        state.functions.push_back(static_cast<int>(aggregate.function));
        // an aggregate of a nullable column also counts its values
        state.nonNull.push_back((aggregate.input != nullptr && aggregate.input->sqlType.nullable) ? state.width++ : 0);
    }
    state.tuples = _table.size();
    state.workers = std::max<size_t>(1, Parallel::threads);
#if COMPILED_LINEAGE
    // the lineage of every reader as an array over the writers, and per writer the least of them: a
    // version is read by all readers iff its time lies below it
    state.set.stride = db._next_branch_id;
    state.minLimits.assign(state.set.stride, std::numeric_limits<uint32_t>::max());
    for (branch_id_t reader : state.set.branches) {
        const auto limits = db.lineageLimits(reader);
        state.set.limits.insert(state.set.limits.end(), limits.begin(), limits.end());
        for (size_t b = 0; b < limits.size(); ++b) state.minLimits[b] = std::min(state.minLimits[b], limits[b]);
    }
#endif
    // sums of doubles: the slots that hold the bits of a double
    state.doubleSums.assign(state.width, 0);
    bool doubleSums = false;
    for (size_t i = 0; i < op.getAggregates().size(); ++i) {
        const auto & aggregate = op.getAggregates()[i];
        if ((aggregate.function == Function::Sum || aggregate.function == Function::Avg) && aggregate.input != nullptr &&
            aggregate.input->sqlType.typeID == SqlType::TypeID::DoubleID) {
            state.doubleSums[1 + i] = 1;
            doubleSums = true;
        }
    }
#if INTERVAL_VISIBILITY || COMPILED_LINEAGE
    // the difference arrays of the shared scan would cancel sums of doubles (1e20 + 1 - 1e20 is 0)
    state.perBranch = doubleSums || (Logical::BranchAggregation::_strategy == Logical::BranchAggregation::Strategy::PerBranch);
#else
    (void) doubleSums;
    // without labels, the readers of a version cannot be told by range tests
    state.perBranch = true;
#endif
    // the parts of the workers lie at least a cache line apart, so that they share none
    auto apart = [](size_t entries, size_t perLine) { return (entries + perLine - 1) / perLine * perLine + perLine; };
    state.groupStride = apart((k + 1) * state.width, 64 / sizeof(int64_t));
    state.scratchStride = apart(k, 64 / sizeof(uint32_t));
    state.direct.resize(state.workers * state.groupStride);
    state.diff.resize(state.workers * state.groupStride);
    state.result.resize(k * state.width);
    state.ptrs.resize(state.workers * state.scratchStride);
    state.lo.resize(state.workers * state.scratchStride);
    state.hi.resize(state.workers * state.scratchStride);
    state.lineages.resize(k, nullptr);
    for (branch_id_t branch : branches) {
        const std::string & name = db._branches.at(branch)->name;
        state.names.push_back({0, 0});
        Sql::storeTextGen(reinterpret_cast<char *>(state.names.back().data()), reinterpret_cast<const uint8_t *>(name.data()), name.size());
    }

    llvm::FunctionType * stateFuncTy = llvm::TypeBuilder<void (void *), false>::get(context);
    llvm::Function * reset = declareFunction("branch_aggregation_reset", stateFuncTy, (void *) &reset_state);
    llvm::Function * finish = declareFunction("branch_aggregation_finish", stateFuncTy, (void *) &finish_state);

    _codeGen->CreateCall(reset, {cg_ptr8_t::fromRawPointer(&state)});
    if (state.tuples > 0) {
        llvm::Function * morsel = genMorselFunction(state);
        llvm::Function * run = declareFunction("branch_aggregation_run",
                llvm::TypeBuilder<void (void *, void *, void *), false>::get(context), (void *) &run_morsels);
        _codeGen->CreateCall(run, {cg_ptr8_t::fromRawPointer(&state).getValue(),
                _codeGen->CreatePointerCast(morsel, LLVMCompat::getInt8PtrTy(context)), _codeGen.getCurrentFunctionGen().getArg(1)});
    }
    _codeGen->CreateCall(finish, {cg_ptr8_t::fromRawPointer(&state)});
    genOutput(state);
}

template<typename T>
llvm::Value * BranchAggregation::genWorkerBase(std::vector<T> & array, size_t perWorker, llvm::Type * elementTy)
{
    return _codeGen->CreateGEP(elementTy, rawPointer(array.data(), elementTy), _codeGen->CreateMul(_worker, cg_size_t(perWorker).getValue()));
}

llvm::Function * BranchAggregation::genMorselFunction(State & state)
{
    auto & context = _codeGen.getLLVMContext();
    llvm::Type * i64Ty = llvm::Type::getInt64Ty(context);
    // generated while the query function is being generated; FunctionGen returns to it afterwards
    FunctionGen funcGen(_codeGen.getCurrentModuleGen(), "branch_aggregation_morsel",
                        llvm::TypeBuilder<void (size_t, size_t, size_t, size_t), false>::get(context));
    _worker = funcGen.getArg(2);
    _position = funcGen.getArg(3);
    _direct = genWorkerBase(state.direct, state.groupStride, i64Ty);
    _diff = genWorkerBase(state.diff, state.groupStride, i64Ty);
    if (state.perBranch) {
        genPerBranchScan(state, funcGen.getArg(0), funcGen.getArg(1));
    } else {
        genSharedScan(state, funcGen.getArg(0), funcGen.getArg(1));
    }
    return funcGen.getFunction();
}

void BranchAggregation::genSharedScan(State & state, llvm::Value * begin, llvm::Value * end)
{
    auto & funcGen = _codeGen.getCurrentFunctionGen();
    auto & context = _codeGen.getLLVMContext();
    llvm::Type * i8PtrTy = LLVMCompat::getInt8PtrTy(context);
    llvm::Type * i32Ty = llvm::Type::getInt32Ty(context);
    llvm::Type * i64Ty = llvm::Type::getInt64Ty(context);
    Database & db = _table.getDatabase();
    const size_t k = state.set.branches.size();
    const bool headsKnown = _table.headsKnown();

    llvm::Function * ranges = declareFunction("branch_aggregation_ranges",
            llvm::TypeBuilder<uint32_t (size_t, void *, void *, size_t, size_t), false>::get(context), (void *) &visible_ranges);
    llvm::Value * ptrs = genWorkerBase(state.ptrs, state.scratchStride, i8PtrTy);
    llvm::Value * los = genWorkerBase(state.lo, state.scratchStride, i32Ty);
    llvm::Value * his = genWorkerBase(state.hi, state.scratchStride, i32Ty);

    LoopGen scanLoop(funcGen, _codeGen->CreateICmpULT(begin, end), {{"index", begin}});
    cg_tid_t tid(scanLoop.getLoopVar(0));
    {
        LoopBodyGen bodyGen(scanLoop);

        auto presence = genPresence(state, tid);
        llvm::Value * allPresent = presence.first;
        IfGen anyPresent(presence.second);
        {
            // the in-place version heads the chain and its label contains the labels of all readers
            llvm::Value * readByAll = llvm::ConstantInt::getFalse(context);
#if COMPILED_LINEAGE
            if (headsKnown) {
                // without labels: the in-place version heads the chain and its time lies below the
                // limit of every reader
                llvm::Value * stamp = LLVMCompat::makeMonotonic(_codeGen->CreateLoad(i64Ty, _codeGen->CreateGEP(i64Ty, rawPointer(_table._head_stamp.data(), i64Ty), tid.getValue())), sizeof(uint64_t));
                llvm::Value * headIsInPlace = _codeGen->CreateICmpNE(stamp, llvm::ConstantInt::get(i64Ty, invalid_stamp));
                llvm::Value * writer = _codeGen->CreateSelect(headIsInPlace, _codeGen->CreateLShr(stamp, 32), llvm::ConstantInt::get(i64Ty, 0));
                llvm::Value * least = _codeGen->CreateLoad(i32Ty, _codeGen->CreateGEP(i32Ty, rawPointer(state.minLimits.data(), i32Ty), writer));
                llvm::Value * covers = _codeGen->CreateICmpULT(_codeGen->CreateTrunc(stamp, i32Ty), least);
                readByAll = _codeGen->CreateAnd(allPresent, _codeGen->CreateAnd(headIsInPlace, covers));
            }
            (void) db;
#else
            if (headsKnown) {
                llvm::Value * heads = rawPointer(_table._head_ident.data(), i32Ty);
                llvm::Value * head = LLVMCompat::makeMonotonic(_codeGen->CreateLoad(i32Ty, _codeGen->CreateGEP(i32Ty, heads, tid.getValue())), sizeof(ident_t));
                llvm::Value * headIsInPlace = _codeGen->CreateICmpNE(head, llvm::ConstantInt::get(i32Ty, invalid_ident));
                llvm::Value * ident = _codeGen->CreateZExt(_codeGen->CreateSelect(headIsInPlace, head, llvm::ConstantInt::get(i32Ty, 0)), i64Ty);
                llvm::Value * l = _codeGen->CreateLoad(i64Ty, _codeGen->CreateGEP(i64Ty, rawPointer(db._identL.data(), i64Ty), ident));
                llvm::Value * r = _codeGen->CreateLoad(i64Ty, _codeGen->CreateGEP(i64Ty, rawPointer(db._identR.data(), i64Ty), ident));
                llvm::Value * covers = _codeGen->CreateAnd(_codeGen->CreateICmpULE(l, cg_size_t(state.set.labels.front()).getValue()),
                                                           _codeGen->CreateICmpULE(cg_size_t(state.set.labels.back()).getValue(), r));
                readByAll = _codeGen->CreateAnd(allPresent, _codeGen->CreateAnd(headIsInPlace, covers));
            }
#endif

            IfGen shared(readByAll);
            {
                auto values = genLoad(tid, nullptr);
                IfGen match(genPredicate(values));
                {
                    genAccumulate(state, cg_size_t(k).getValue(), values);
                }
                match.EndIf();
            }
            shared.Else();
            {
                // one walk of the chain splits the readers into ranges that see the same version
                llvm::Value * parts = _codeGen->CreateZExt(_codeGen->CreateCall(ranges, {tid.getValue(),
                        cg_ptr8_t::fromRawPointer(&_table).getValue(), cg_ptr8_t::fromRawPointer(&state).getValue(),
                        _codeGen->CreateZExt(allPresent, i64Ty), _worker}), i64Ty);
                LoopGen partLoop(funcGen, _codeGen->CreateICmpUGT(parts, cg_size_t(0ul).getValue()), {{"part", cg_size_t(0ul)}});
                cg_size_t part(partLoop.getLoopVar(0));
                {
                    LoopBodyGen partBody(partLoop);
                    llvm::Value * ptr = _codeGen->CreateLoad(i8PtrTy, _codeGen->CreateGEP(i8PtrTy, ptrs, part.getValue()));
                    llvm::Value * lo = _codeGen->CreateZExt(_codeGen->CreateLoad(i32Ty, _codeGen->CreateGEP(i32Ty, los, part.getValue())), i64Ty);
                    llvm::Value * hi = _codeGen->CreateZExt(_codeGen->CreateLoad(i32Ty, _codeGen->CreateGEP(i32Ty, his, part.getValue())), i64Ty);
                    auto values = genLoad(tid, ptr);
                    IfGen match(genPredicate(values));
                    {
                        genAccumulateRange(state, lo, hi, values);
                    }
                    match.EndIf();
                }
                cg_size_t nextPart = part + 1ul;
                partLoop.loopDone(_codeGen->CreateICmpULT(nextPart.getValue(), parts), {nextPart});
            }
            shared.EndIf();
        }
        anyPresent.EndIf();
    }
    cg_size_t nextIndex = tid + 1ul;
    scanLoop.loopDone(_codeGen->CreateICmpULT(nextIndex.getValue(), end), {nextIndex});
}

void BranchAggregation::genPerBranchScan(State & state, llvm::Value * begin, llvm::Value * end)
{
    auto & funcGen = _codeGen.getCurrentFunctionGen();
    auto & context = _codeGen.getLLVMContext();
    llvm::Type * i8PtrTy = LLVMCompat::getInt8PtrTy(context);
    llvm::Type * i32Ty = llvm::Type::getInt32Ty(context);
    llvm::Type * i64Ty = llvm::Type::getInt64Ty(context);

    llvm::Function * latest = declareFunction("branch_aggregation_latest_entry",
            llvm::TypeBuilder<void * (size_t, void *, void *, size_t), false>::get(context), (void *) &latest_entry);

    // the morsel belongs to the branch at _position
    llvm::Value * branch = _codeGen->CreateLoad(i32Ty, _codeGen->CreateGEP(i32Ty, rawPointer(state.set.branches.data(), i32Ty), _position));
    llvm::Value * isMaster = _codeGen->CreateICmpEQ(branch, llvm::ConstantInt::get(i32Ty, master_branch_id));
#if COMPILED_LINEAGE
    const bool headsKnown = _table.headsKnown();
    // the limits of the reader at _position
    llvm::Value * readerLimits = _codeGen->CreateGEP(i32Ty, rawPointer(state.set.limits.data(), i32Ty),
                                                     _codeGen->CreateMul(_position, cg_size_t(state.set.stride).getValue()));
#endif
#if INTERVAL_VISIBILITY
    const bool headsKnown = _table.headsKnown();
    Database & db = _table.getDatabase();
    llvm::Value * readerLabel = _codeGen->CreateLoad(i64Ty, _codeGen->CreateGEP(i64Ty, rawPointer(state.set.labels.data(), i64Ty), _position));
#endif

    LoopGen scanLoop(funcGen, _codeGen->CreateICmpULT(begin, end), {{"index", begin}});
    cg_tid_t tid(scanLoop.getLoopVar(0));
    {
        LoopBodyGen bodyGen(scanLoop);
        IfGen visible(genIsInBranch(tid, branch));
        {
            // as in TableScan: main reads in place; a branch reads in place if the in-place
            // version heads the chain and is visible, and walks the chain otherwise
            llvm::Value * walk = _codeGen->CreateNot(isMaster);
#if INTERVAL_VISIBILITY
            if (headsKnown) {
                llvm::Value * head = LLVMCompat::makeMonotonic(_codeGen->CreateLoad(i32Ty, _codeGen->CreateGEP(i32Ty, rawPointer(_table._head_ident.data(), i32Ty), tid.getValue())), sizeof(ident_t));
                llvm::Value * headIsInPlace = _codeGen->CreateICmpNE(head, llvm::ConstantInt::get(i32Ty, invalid_ident));
                llvm::Value * ident = _codeGen->CreateZExt(_codeGen->CreateSelect(headIsInPlace, head, llvm::ConstantInt::get(i32Ty, 0)), i64Ty);
                llvm::Value * l = _codeGen->CreateLoad(i64Ty, _codeGen->CreateGEP(i64Ty, rawPointer(db._identL.data(), i64Ty), ident));
                llvm::Value * r = _codeGen->CreateLoad(i64Ty, _codeGen->CreateGEP(i64Ty, rawPointer(db._identR.data(), i64Ty), ident));
                llvm::Value * inPlace = _codeGen->CreateAnd(headIsInPlace,
                        _codeGen->CreateAnd(_codeGen->CreateICmpULE(l, readerLabel), _codeGen->CreateICmpULE(readerLabel, r)));
                walk = _codeGen->CreateAnd(walk, _codeGen->CreateNot(inPlace));
            }
#elif COMPILED_LINEAGE
            if (headsKnown) {
                llvm::Value * stamp = LLVMCompat::makeMonotonic(_codeGen->CreateLoad(i64Ty, _codeGen->CreateGEP(i64Ty, rawPointer(_table._head_stamp.data(), i64Ty), tid.getValue())), sizeof(uint64_t));
                llvm::Value * headIsInPlace = _codeGen->CreateICmpNE(stamp, llvm::ConstantInt::get(i64Ty, invalid_stamp));
                llvm::Value * writer = _codeGen->CreateSelect(headIsInPlace, _codeGen->CreateLShr(stamp, 32), llvm::ConstantInt::get(i64Ty, 0));
                llvm::Value * limit = _codeGen->CreateLoad(i32Ty, _codeGen->CreateGEP(i32Ty, readerLimits, writer));
                llvm::Value * inPlace = _codeGen->CreateAnd(headIsInPlace, _codeGen->CreateICmpULT(_codeGen->CreateTrunc(stamp, i32Ty), limit));
                walk = _codeGen->CreateAnd(walk, _codeGen->CreateNot(inPlace));
            }
#endif
            IfGen chain(funcGen, walk, {{"latest", cg_size_t(0ul)}});
            {
                llvm::Value * result = _codeGen->CreateCall(latest, {tid.getValue(), cg_ptr8_t::fromRawPointer(&_table).getValue(),
                        cg_ptr8_t::fromRawPointer(&state).getValue(), _position});
                chain.setVar(0, _codeGen->CreatePtrToInt(result, i64Ty));
            }
            chain.EndIf();
            llvm::Value * latestEntry = chain.getResult(0); // each call creates a phi node
            llvm::Value * ptr = _codeGen->CreateIntToPtr(latestEntry, i8PtrTy);

#if LAZY_MEMBERSHIP
            // the branch contains tuples of its parent whose versions it does not see
            IfGen seesVersion(_codeGen->CreateICmpNE(latestEntry, llvm::ConstantInt::get(i64Ty, 1)));
#endif
            auto values = genLoad(tid, ptr);
            IfGen match(genPredicate(values));
            {
                genAccumulate(state, _position, values);
            }
            match.EndIf();
#if LAZY_MEMBERSHIP
            seesVersion.EndIf();
#endif
        }
        visible.EndIf();
    }
    cg_size_t nextIndex = tid + 1ul;
    scanLoop.loopDone(_codeGen->CreateICmpULT(nextIndex.getValue(), end), {nextIndex});
}

void BranchAggregation::genOutput(State & state)
{
    auto & op = static_cast<const Logical::BranchAggregation &>(_logicalOperator);
    auto & funcGen = _codeGen.getCurrentFunctionGen();
    auto & context = _codeGen.getLLVMContext();
    llvm::Type * i8PtrTy = LLVMCompat::getInt8PtrTy(context);
    llvm::Type * i32Ty = llvm::Type::getInt32Ty(context);
    llvm::Type * i64Ty = llvm::Type::getInt64Ty(context);
    const size_t k = state.set.branches.size();

    LoopGen outputLoop(funcGen, {{"group", cg_size_t(0ul)}});
    cg_size_t group(outputLoop.getLoopVar(0));
    {
        LoopBodyGen outputBody(outputLoop);
        llvm::Value * count = _codeGen->CreateLoad(i64Ty, genSlotPtr(rawPointer(state.result.data(), i64Ty), group.getValue(), state.width, 0));
        // GROUP BY: a branch without qualifying tuples forms no group
        IfGen nonEmpty(_codeGen->CreateICmpSGT(count, cg_size_t(0ul).getValue()));
        {
            iu_value_mapping_t values;
            std::vector<value_op_t> outputs;
            if (op.getBranchIU() != nullptr) {
                llvm::Type * i8Ty = llvm::Type::getInt8Ty(context);
                llvm::Value * name = _codeGen->CreateGEP(i8Ty, rawPointer(reinterpret_cast<uint8_t *>(state.names.data()), i8Ty),
                                                         _codeGen->CreateMul(group.getValue(), cg_size_t(sizeof(state.names[0])).getValue()));
                outputs.push_back(Text::fromRawValues({name}, op.getBranchIU()->sqlType));
                values[op.getBranchIU()] = outputs.back().get();
            }
            const auto & aggregates = op.getAggregates();
            for (size_t i = 0; i < aggregates.size(); ++i) {
                llvm::Value * raw = _codeGen->CreateLoad(i64Ty, genSlotPtr(rawPointer(state.result.data(), i64Ty), group.getValue(), state.width, 1 + i));
                const SqlType type = aggregates[i].produced->sqlType;
                if (type.typeID == SqlType::TypeID::IntegerID) {
                    raw = _codeGen->CreateTrunc(raw, i32Ty);
                } else if (type.typeID == SqlType::TypeID::DoubleID) {
                    if (aggregates[i].function == Function::Min || aggregates[i].function == Function::Max) {
                        llvm::Value * negative = _codeGen->CreateICmpSLT(raw, llvm::ConstantInt::get(i64Ty, 0));
                        raw = _codeGen->CreateSelect(negative, _codeGen->CreateXor(raw, llvm::ConstantInt::get(i64Ty, INT64_MAX)), raw);
                    }
                    raw = _codeGen->CreateBitCast(raw, llvm::Type::getDoubleTy(context));
                }
                outputs.push_back(Value::fromRawValues({raw}, toNotNullableTy(type)));
                if (type.nullable) {
                    // NULL without values
                    llvm::Value * count = _codeGen->CreateLoad(i64Ty, genSlotPtr(rawPointer(state.result.data(), i64Ty), group.getValue(), state.width, state.nonNull[i]));
                    outputs.back() = NullableValue::create(*outputs.back(), cg_bool_t(_codeGen->CreateICmpEQ(count, cg_size_t(0ul).getValue())));
                }
                values[aggregates[i].produced] = outputs.back().get();
            }
            emit(values);
        }
        nonEmpty.EndIf();
    }
    cg_size_t nextGroup = group + 1ul;
    outputLoop.loopDone(nextGroup < k, {nextGroup});
}

llvm::Value * BranchAggregation::genIsInBranch(cg_tid_t tid, llvm::Value * branch)
{
#if LAZY_MEMBERSHIP
    return genMembershipTest(_table.getMembership(), tid, branch).llvmValue;
#else
    auto & context = _codeGen.getLLVMContext();
    auto & bitmap = _table.getMembership().bitmap();
    llvm::Type * i8Ty = llvm::Type::getInt8Ty(context);
    llvm::Type * i64Ty = llvm::Type::getInt64Ty(context);
    llvm::Value * column = _codeGen->CreateZExt(branch, i64Ty);
    llvm::Value * byteIndex = _codeGen->CreateAdd(_codeGen->CreateMul(tid.getValue(), cg_size_t(bitmap.getRowSize()).getValue()),
                                                  _codeGen->CreateLShr(column, 3));
    llvm::Value * byte = _codeGen->CreateLoad(i8Ty, _codeGen->CreateGEP(i8Ty, rawPointer(static_cast<uint8_t *>(bitmap.data()), i8Ty), byteIndex));
    llvm::Value * shift = _codeGen->CreateTrunc(_codeGen->CreateAnd(column, 7), i8Ty);
    return _codeGen->CreateTrunc(_codeGen->CreateLShr(byte, shift), llvm::Type::getInt1Ty(context));
#endif
}

std::pair<llvm::Value *, llvm::Value *> BranchAggregation::genPresence(State & state, cg_tid_t tid)
{
    auto & context = _codeGen.getLLVMContext();
    llvm::Type * i8Ty = llvm::Type::getInt8Ty(context);
#if LAZY_MEMBERSHIP
    // a tuple that no branch treats differently from the root is in all readers or in none;
    // otherwise get_visible_ranges() tests the readers one by one
    llvm::Value * states = rawPointer(const_cast<uint8_t *>(_table.getMembership().states()), i8Ty);
    llvm::Value * tupleState = _codeGen->CreateLoad(i8Ty, _codeGen->CreateGEP(i8Ty, states, tid.getValue()));
    llvm::Value * zero = llvm::ConstantInt::get(i8Ty, 0);
    const bool readsRoot = std::find(state.set.branches.begin(), state.set.branches.end(), master_branch_id) != state.set.branches.end();
    const bool readsOthers = state.set.branches.size() > (readsRoot ? 1u : 0u);
    llvm::Value * inRoot = _codeGen->CreateICmpNE(_codeGen->CreateAnd(tupleState, BranchMembership::inRoot), zero);
    llvm::Value * inBranches = _codeGen->CreateICmpNE(_codeGen->CreateAnd(tupleState, BranchMembership::inBranches), zero);
    llvm::Value * isDiverged = _codeGen->CreateICmpNE(_codeGen->CreateAnd(tupleState, BranchMembership::diverged), zero);
    llvm::Value * all = _codeGen->CreateNot(isDiverged);
    llvm::Value * any = isDiverged;
    if (readsRoot) {
        all = _codeGen->CreateAnd(all, inRoot);
        any = _codeGen->CreateOr(any, inRoot);
    }
    if (readsOthers) {
        all = _codeGen->CreateAnd(all, inBranches);
        any = _codeGen->CreateOr(any, inBranches);
    }
    return {all, any};
#else
    auto & bitmap = _table.getMembership().bitmap();
    const size_t rowSize = bitmap.getRowSize();
    const size_t chunkBytes = (rowSize % 8 == 0) ? 8 : 1;
    const size_t chunkBits = 8 * chunkBytes;

    // the bits of the readers in each chunk of a bitmap row
    std::vector<uint64_t> masks(rowSize / chunkBytes, 0);
    for (branch_id_t branch : state.set.branches) {
        masks[branch / chunkBits] |= uint64_t(1) << (branch % chunkBits);
    }

    llvm::Type * chunkTy = llvm::Type::getIntNTy(context, chunkBits);
    llvm::Value * row = _codeGen->CreateGEP(i8Ty, rawPointer(static_cast<uint8_t *>(bitmap.data()), i8Ty),
                                            _codeGen->CreateMul(tid.getValue(), cg_size_t(rowSize).getValue()));
    llvm::Value * all = llvm::ConstantInt::getTrue(context);
    llvm::Value * any = llvm::ConstantInt::getFalse(context);
    for (size_t chunk = 0; chunk < masks.size(); ++chunk) {
        if (masks[chunk] == 0) continue;
        llvm::Value * chunkPtr = _codeGen->CreatePointerCast(_codeGen->CreateGEP(i8Ty, row, cg_size_t(chunk * chunkBytes).getValue()), llvm::PointerType::getUnqual(chunkTy));
        llvm::Value * mask = llvm::ConstantInt::get(chunkTy, masks[chunk]);
        llvm::Value * bits = _codeGen->CreateAnd(_codeGen->CreateLoad(chunkTy, chunkPtr), mask);
        all = _codeGen->CreateAnd(all, _codeGen->CreateICmpEQ(bits, mask));
        any = _codeGen->CreateOr(any, _codeGen->CreateICmpNE(bits, llvm::ConstantInt::get(chunkTy, 0)));
    }
    return {all, any};
#endif
}

iu_value_mapping_t BranchAggregation::genLoad(cg_tid_t tid, llvm::Value * tuplePtr)
{
    iu_value_mapping_t values;
    llvm::Value * isInPlace = nullptr;
    if (tuplePtr != nullptr) {
        isInPlace = _codeGen->CreateIsNull(tuplePtr);
    }
    for (auto & [iu, ci, index, columnTy, columnPtr] : _columns) {
        llvm::Value * elemPtr = _codeGen->CreateGEP(columnTy, columnPtr, {cg_size_t(0ul), tid});
        if (tuplePtr != nullptr) {
            llvm::Value * versionPtr = genVersionAttributePtr(_table, tuplePtr, index);
            elemPtr = _codeGen->CreateSelect(isInPlace, elemPtr, versionPtr);
        }
        _loaded.push_back(Value::load(elemPtr, ci->type));
        values[iu] = _loaded.back().get();
    }
    return values;
}

llvm::Value * BranchAggregation::genPredicate(const iu_value_mapping_t & values)
{
    llvm::Value * match = llvm::ConstantInt::getTrue(_codeGen.getLLVMContext());
    for (auto & predicate : _predicates) {
        value_op_t result = predicate->evaluate(values);
        match = _codeGen->CreateAnd(match, Sql::Utils::isTrue(*result));
    }
    return match;
}

llvm::Value * BranchAggregation::genInputPresent(const iu_value_mapping_t & values, size_t aggregate)
{
    auto & op = static_cast<const Logical::BranchAggregation &>(_logicalOperator);
    iu_p_t input = op.getAggregates()[aggregate].input;
    if (input == nullptr || !input->sqlType.nullable) return nullptr;
    return (!static_cast<const NullableValue &>(*values.at(input)).isNull()).llvmValue;
}

llvm::Value * BranchAggregation::genInputValue(const iu_value_mapping_t & values, size_t aggregate)
{
    auto & op = static_cast<const Logical::BranchAggregation &>(_logicalOperator);
    const Value & value = getAssociatedValue(*values.at(op.getAggregates()[aggregate].input));
    llvm::Value * raw = value.getLLVMValue();
    llvm::Type * i64Ty = llvm::Type::getInt64Ty(_codeGen.getLLVMContext());
    if (raw->getType()->isDoubleTy()) {
        Function function = op.getAggregates()[aggregate].function;
        // a NaN as the greatest double (as PostgreSQL), then the bits
        llvm::Value * isNaN = _codeGen->CreateFCmpUNO(raw, raw);
        raw = _codeGen->CreateSelect(isNaN, llvm::ConstantFP::getNaN(raw->getType()), raw);
        llvm::Value * bits = _codeGen->CreateBitCast(raw, i64Ty);
        if (function == Function::Min || function == Function::Max) {
            // ordered as the doubles: the negative ones with their other bits flipped
            llvm::Value * negative = _codeGen->CreateICmpSLT(bits, llvm::ConstantInt::get(i64Ty, 0));
            bits = _codeGen->CreateSelect(negative, _codeGen->CreateXor(bits, llvm::ConstantInt::get(i64Ty, INT64_MAX)), bits);
        }
        return bits;
    }
    if (raw->getType()->getIntegerBitWidth() < 64) {
        raw = _codeGen->CreateSExt(raw, llvm::Type::getInt64Ty(_codeGen.getLLVMContext()));
    }
    return raw;
}

llvm::Value * BranchAggregation::genSlotPtr(llvm::Value * base, llvm::Value * group, size_t width, size_t slot)
{
    llvm::Type * i64Ty = llvm::Type::getInt64Ty(_codeGen.getLLVMContext());
    llvm::Value * index = _codeGen->CreateAdd(_codeGen->CreateMul(group, cg_size_t(width).getValue()), cg_size_t(slot).getValue());
    return _codeGen->CreateGEP(i64Ty, base, index);
}

void BranchAggregation::genAccumulate(State & state, llvm::Value * group, const iu_value_mapping_t & values)
{
    auto & op = static_cast<const Logical::BranchAggregation &>(_logicalOperator);
    llvm::Type * i64Ty = llvm::Type::getInt64Ty(_codeGen.getLLVMContext());

    llvm::Value * countPtr = genSlotPtr(_direct, group, state.width, 0);
    _codeGen->CreateStore(_codeGen->CreateAdd(_codeGen->CreateLoad(i64Ty, countPtr), cg_size_t(1ul).getValue()), countPtr);
    const auto & aggregates = op.getAggregates();
    for (size_t i = 0; i < aggregates.size(); ++i) {
        // NULLs are skipped: they count as no value, add 0 and change no extreme
        llvm::Value * present = genInputPresent(values, i);
        if (present != nullptr) {
            llvm::Value * nonNullPtr = genSlotPtr(_direct, group, state.width, state.nonNull[i]);
            _codeGen->CreateStore(_codeGen->CreateAdd(_codeGen->CreateLoad(i64Ty, nonNullPtr), _codeGen->CreateZExt(present, i64Ty)), nonNullPtr);
        }
        if (aggregates[i].function == Function::Count) continue;
        llvm::Value * slotPtr = genSlotPtr(_direct, group, state.width, 1 + i);
        llvm::Value * current = _codeGen->CreateLoad(i64Ty, slotPtr);
        llvm::Value * value = genInputValue(values, i);
        llvm::Value * updated;
        switch (aggregates[i].function) {
            case Function::Min: updated = _codeGen->CreateSelect(_codeGen->CreateICmpSLT(value, current), value, current); break;
            case Function::Max: updated = _codeGen->CreateSelect(_codeGen->CreateICmpSGT(value, current), value, current); break;
            default:
                if (state.doubleSums[1 + i]) {
                    llvm::Type * doubleTy = llvm::Type::getDoubleTy(_codeGen.getLLVMContext());
                    updated = _codeGen->CreateBitCast(_codeGen->CreateFAdd(_codeGen->CreateBitCast(current, doubleTy),
                                                                           _codeGen->CreateBitCast(value, doubleTy)), i64Ty);
                } else {
                    updated = _codeGen->CreateAdd(current, value);
                }
        }
        if (present != nullptr) updated = _codeGen->CreateSelect(present, updated, current);
        _codeGen->CreateStore(updated, slotPtr);
    }
}

void BranchAggregation::genAccumulateRange(State & state, llvm::Value * lo, llvm::Value * hi, const iu_value_mapping_t & values)
{
    auto & op = static_cast<const Logical::BranchAggregation &>(_logicalOperator);
    auto & funcGen = _codeGen.getCurrentFunctionGen();
    llvm::Type * i64Ty = llvm::Type::getInt64Ty(_codeGen.getLLVMContext());
    const auto & aggregates = op.getAggregates();
    llvm::Value * end = _codeGen->CreateAdd(hi, cg_size_t(1ul).getValue());

    // count and sum: +value at the first reader of the range, -value after the last one
    auto addRange = [&](size_t slot, llvm::Value * value) {
        llvm::Value * first = genSlotPtr(_diff, lo, state.width, slot);
        _codeGen->CreateStore(_codeGen->CreateAdd(_codeGen->CreateLoad(i64Ty, first), value), first);
        llvm::Value * after = genSlotPtr(_diff, end, state.width, slot);
        _codeGen->CreateStore(_codeGen->CreateSub(_codeGen->CreateLoad(i64Ty, after), value), after);
    };
    addRange(0, cg_size_t(1ul).getValue());
    bool extremes = false;
    for (size_t i = 0; i < aggregates.size(); ++i) {
        // NULLs are skipped: they count as no value and add 0
        llvm::Value * present = genInputPresent(values, i);
        if (present != nullptr) {
            addRange(state.nonNull[i], _codeGen->CreateZExt(present, i64Ty));
        }
        switch (aggregates[i].function) {
            case Function::Sum:
            case Function::Avg: {
                llvm::Value * value = genInputValue(values, i);
                if (present != nullptr) value = _codeGen->CreateSelect(present, value, cg_size_t(0ul).getValue());
                addRange(1 + i, value);
                break;
            }
            case Function::Min:
            case Function::Max: extremes = true; break;
            default: break;
        }
    }
    if (!extremes) return;

    // min and max cannot be undone: update every reader of the range
    LoopGen readerLoop(funcGen, {{"reader", lo}});
    llvm::Value * reader = readerLoop.getLoopVar(0);
    {
        LoopBodyGen readerBody(readerLoop);
        for (size_t i = 0; i < aggregates.size(); ++i) {
            if (aggregates[i].function != Function::Min && aggregates[i].function != Function::Max) continue;
            llvm::Value * slotPtr = genSlotPtr(_direct, reader, state.width, 1 + i);
            llvm::Value * current = _codeGen->CreateLoad(i64Ty, slotPtr);
            llvm::Value * value = genInputValue(values, i);
            llvm::Value * better = (aggregates[i].function == Function::Min) ? _codeGen->CreateICmpSLT(value, current) : _codeGen->CreateICmpSGT(value, current);
            if (llvm::Value * present = genInputPresent(values, i)) better = _codeGen->CreateAnd(better, present);
            _codeGen->CreateStore(_codeGen->CreateSelect(better, value, current), slotPtr);
        }
    }
    llvm::Value * nextReader = _codeGen->CreateAdd(reader, cg_size_t(1ul).getValue());
    readerLoop.loopDone(_codeGen->CreateICmpULE(nextReader, hi), {nextReader});
}

} // end namespace Physical
} // end namespace Algebra
