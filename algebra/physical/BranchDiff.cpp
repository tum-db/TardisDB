#include "algebra/physical/BranchDiff.hpp"

#include <algorithm>
#include <cstddef>

#include "codegen/LLVMCompat.hpp"

#include "sql/SqlTuple.hpp"
#include "sql/SqlUtils.hpp"

using namespace Sql;

namespace Sql {
void storeTextGen(char *dest, const uint8_t * bytes, size_t len);
}

namespace Algebra {
namespace Physical {

using State = BranchDiff::State;

//-----------------------------------------------------------------------------
// Runtime functions called by the compiled query

static size_t diff_run(State * state)
{
    state->entries.clear();
    diff_branches(*state->table, state->branches[0], state->branches[1], state->entries);
    return state->entries.size();
}

static void * diff_entries(State * state)
{
    return state->entries.data();
}

static llvm::Function * declareFunction(const std::string & name, llvm::FunctionType * type, void * function)
{
    auto & moduleGen = getThreadLocalCodeGen().getCurrentModuleGen();
    llvm::Function * func = LLVMCompat::getOrInsertFunction(moduleGen.getModule(), name, type);
    moduleGen.addFunctionMapping(func, function);
    return func;
}

//-----------------------------------------------------------------------------
// BranchDiff

BranchDiff::BranchDiff(const logical_operator_t & logicalOperator, std::vector<Expressions::exp_op_t> predicates, QueryContext & queryContext) :
        NullaryOperator(logicalOperator, queryContext),
        _table(static_cast<const Logical::BranchDiff &>(logicalOperator).getTable()),
        _predicates(std::move(predicates))
{
    auto & op = static_cast<const Logical::BranchDiff &>(logicalOperator);
    if (_table.size() < 1) return; // nothing to compare

    for (iu_p_t iu : op.getProduced()) {
        if (iu->iuType != InformationUnit::Type::ColumnRef) continue; // the name of the branch
        ci_p_t ci = getColumnInformation(iu);
        size_t index = ci->index;
        llvm::Type * columnTy = llvm::ArrayType::get(toLLVMTy(ci->type), _table.size());
        llvm::Value * columnPtr = createPointerValue(ci->column->front(), columnTy);
        _columns.emplace_back(iu, ci, index, columnTy, columnPtr);
    }
}

BranchDiff::~BranchDiff()
{ }

void BranchDiff::produce()
{
    auto & op = static_cast<const Logical::BranchDiff &>(_logicalOperator);
    if (_table.size() < 1) return;

    auto & funcGen = _codeGen.getCurrentFunctionGen();
    auto & context = _codeGen.getLLVMContext();
    llvm::Type * i8Ty = llvm::Type::getInt8Ty(context);
    llvm::Type * i8PtrTy = LLVMCompat::getInt8PtrTy(context);
    llvm::Type * i32Ty = llvm::Type::getInt32Ty(context);
    llvm::Type * i64Ty = llvm::Type::getInt64Ty(context);
    Database & db = _table.getDatabase();

    auto owned = std::make_unique<State>();
    State & state = *owned;
    _context.executionContext.acquireResource(std::move(owned));
    state.table = &_table;
    state.branches[0] = op.getFirst();
    state.branches[1] = op.getSecond();
    for (int side = 0; side < 2; ++side) {
        const std::string & name = db._branches.at(state.branches[side])->name;
        state.names[side] = {0, 0};
        Sql::storeTextGen(reinterpret_cast<char *>(state.names[side].data()), reinterpret_cast<const uint8_t *>(name.data()), name.size());
    }

    llvm::Function * run = declareFunction("branch_diff_run", llvm::TypeBuilder<size_t (void *), false>::get(context), (void *) &diff_run);
    llvm::Function * entriesFunc = declareFunction("branch_diff_entries", llvm::TypeBuilder<void * (void *), false>::get(context), (void *) &diff_entries);
    llvm::Value * statePtr = cg_ptr8_t::fromRawPointer(&state).getValue();
    llvm::Value * count = _codeGen->CreateCall(run, {statePtr});
    llvm::Value * entries = _codeGen->CreateCall(entriesFunc, {statePtr});
    llvm::Value * names = cg_ptr8_t::fromRawPointer(state.names.data()).getValue();

    LoopGen entryLoop(funcGen, _codeGen->CreateICmpULT(cg_size_t(0ul).getValue(), count), {{"entry", cg_size_t(0ul)}});
    cg_size_t entry(entryLoop.getLoopVar(0));
    {
        LoopBodyGen bodyGen(entryLoop);
        llvm::Value * entryPtr = _codeGen->CreateGEP(i8Ty, entries, _codeGen->CreateMul(entry.getValue(), cg_size_t(sizeof(DiffEntry)).getValue()));
        auto field = [&](size_t offset, llvm::Type * type) {
            llvm::Value * ptr = _codeGen->CreateGEP(i8Ty, entryPtr, cg_size_t(offset).getValue());
            return _codeGen->CreateLoad(type, _codeGen->CreatePointerCast(ptr, llvm::PointerType::getUnqual(type)));
        };
        cg_tid_t tid(field(offsetof(DiffEntry, tid), i64Ty));
        llvm::Value * data = field(offsetof(DiffEntry, data), i8PtrTy);
        llvm::Value * side = _codeGen->CreateZExt(field(offsetof(DiffEntry, side), i32Ty), i64Ty);

        iu_value_mapping_t values = genLoad(tid, data);
        llvm::Value * match = llvm::ConstantInt::getTrue(context);
        for (auto & predicate : _predicates) {
            value_op_t result = predicate->evaluate(values);
            match = _codeGen->CreateAnd(match, Sql::Utils::isTrue(*result));
        }
        IfGen matches(match);
        {
            value_op_t branchName;
            for (iu_p_t iu : op.getProduced()) {
                if (iu->iuType == InformationUnit::Type::ColumnRef) continue;
                llvm::Value * name = _codeGen->CreateGEP(i8Ty, names, _codeGen->CreateMul(side, cg_size_t(sizeof(state.names[0])).getValue()));
                branchName = Text::fromRawValues({name}, iu->sqlType);
                values[iu] = branchName.get();
            }
            _parent->consume(values, *this);
        }
        matches.EndIf();
    }
    cg_size_t nextEntry = entry + 1ul;
    entryLoop.loopDone(_codeGen->CreateICmpULT(nextEntry.getValue(), count), {nextEntry});
}

iu_value_mapping_t BranchDiff::genLoad(cg_tid_t tid, llvm::Value * tuplePtr)
{
    iu_value_mapping_t values;
    llvm::Value * isInPlace = _codeGen->CreateIsNull(tuplePtr);
    for (auto & [iu, ci, index, columnTy, columnPtr] : _columns) {
        llvm::Value * elemPtr = _codeGen->CreateGEP(columnTy, columnPtr, {cg_size_t(0ul), tid});
        llvm::Value * versionPtr = genVersionAttributePtr(_table, tuplePtr, index);
        elemPtr = _codeGen->CreateSelect(isInPlace, elemPtr, versionPtr);
        _loaded.push_back(Value::load(elemPtr, ci->type));
        values[iu] = _loaded.back().get();
    }
    return values;
}

} // end namespace Physical
} // end namespace Algebra
