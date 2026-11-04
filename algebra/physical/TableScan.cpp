//
// Created by josef on 31.12.16.
//

#include "algebra/physical/TableScan.hpp"

#include "codegen/LLVMCompat.hpp"
#include "foundations/version_management.hpp"
#include <unordered_map>

#include "sql/SqlTuple.hpp"
#include "algebra/physical/Print.hpp"
#include "algebra/physical/Select.hpp"
#include "algebra/physical/TupleStream.hpp"
#include "foundations/Parallel.hpp"

using namespace Sql;

namespace Algebra {
namespace Physical {

#if USE_DATA_VERSIONING
static void scan_run(void * state, void * function, void * context)
{
    auto * s = static_cast<TableScan::ParallelState *>(state);
#if !INTERVAL_VISIBILITY
    // the lineage maps are built after the query has been compiled
    auto & executionContext = static_cast<QueryContext *>(context)->executionContext;
    auto it = executionContext.branch_lineages.find(s->branch);
    s->lineage = (it != executionContext.branch_lineages.end()) ? &it->second : nullptr;
#endif
    Parallel::runMorsels(Parallel::threads, s->tuples, 1, reinterpret_cast<Parallel::morsel_function_t>(function));
}

// copies the candidates, since updates of the scanned tuples may add to the index
static size_t index_probe(void * state)
{
    auto * s = static_cast<TableScan::ProbeState *>(state);
    s->candidates.clear();
    if (const auto * tids = s->index->lookup(s->keyHash)) {
        s->candidates = *tids;
    }
    return s->candidates.size();
}

static void * index_candidates(void * state)
{
    return static_cast<TableScan::ProbeState *>(state)->candidates.data();
}

// get_latest_entry writes the reader into the shared context, which workers must not do
static const void * scan_latest_entry(size_t tid, void * state)
{
    auto * s = static_cast<TableScan::ParallelState *>(state);
    return get_latest_entry_of_reader(tid, *s->table, s->branch, s->label, s->lineage);
}
#endif

TableScan::TableScan(const logical_operator_t & logicalOperator, Table & table, branch_id_t branchId, QueryContext &queryContext) :
        NullaryOperator(std::move(logicalOperator), queryContext),
        table(table),
        branchId(branchId)
{
    // collect all information which is necessary to access the columns
    for (auto iu : getRequired()) {
        auto ci = getColumnInformation(iu);
        if (ci->columnName.compare("tid") == 0) {
            continue;
        }

        SqlType storedSqlType;
        if (ci->type.nullable) {
            // the null indicator will be stored in the NullIndicatorTable
            storedSqlType = toNotNullableTy( ci->type );
        } else {
            storedSqlType = ci->type;
        }

        size_t tableSize = table.size();
        llvm::Type * elemTy = toLLVMTy(storedSqlType);
        llvm::Type * columnTy = llvm::ArrayType::get(elemTy, tableSize);
        llvm::Value * columnPtr = createPointerValue(ci->column->front(), columnTy);
        size_t columnIndex = 0;
        for (int i = 0; i<table.getColumnCount(); i++) {
            if (table.getColumnNames()[i].compare(ci->columnName) == 0) {
                columnIndex = i;
                break;
            }
        }

        columns.emplace_back(ci, columnTy, columnPtr, columnIndex, nullptr);
    }
}

TableScan::~TableScan()
{ }

void TableScan::produce()
{
    auto & funcGen = _codeGen.getCurrentFunctionGen();

    size_t tableSize = table.size();
    if (tableSize < 1) return;  // nothing to produce
#if USE_DATA_VERSIONING
    prepare_scan(table, branchId);
    auto & logicalScan = static_cast<const Logical::TableScan &>(_logicalOperator);
    if (logicalScan.getIndex() != nullptr) {
        produceIndexProbe(*logicalScan.getIndex(), logicalScan.getIndexKeyHash());
        return;
    }
    if (Parallel::threads > 1 && isParallelPipeline()) {
        produceParallel();
        return;
    }
#endif

    // iterate over all tuples
#ifdef __APPLE__
    LoopGen scanLoop(funcGen, {{"index", cg_size_t(0ull)}});
#else
    LoopGen scanLoop(funcGen, {{"index", cg_size_t(0ul)}});
#endif
    cg_size_t tid(scanLoop.getLoopVar(0));
    {
        LoopBodyGen bodyGen(scanLoop);

#if USE_DATA_VERSIONING
        IfGen visibilityCheck(isVisible(tid, branchId));
        {
            produce(tid, branchId);
        }
        visibilityCheck.EndIf();
#else
        produce(tid);
#endif
    }
    cg_size_t nextIndex = tid + 1ul;
    scanLoop.loopDone(nextIndex < tableSize, {nextIndex});
}


#if USE_DATA_VERSIONING
void TableScan::produce(cg_tid_t tid, branch_id_t branchId) {
    iu_value_mapping_t values;

    // get null indicator column data
    auto & nullIndicatorTable = table.getNullIndicatorTable();
    iu_set_t required = getRequired();

    cg_voidptr_t resultPtr;
    cg_bool_t ptrIsNotNull(false);
    std::unique_ptr<IfGen> seesVersion;
    if (branchId != master_branch_id) {
        resultPtr = genGetLatestEntryCall(tid,branchId);
        ptrIsNotNull = nullPointerCheck(resultPtr);
#if LAZY_MEMBERSHIP
        // the branch contains tuples of its parent whose versions it does not see
        seesVersion = std::make_unique<IfGen>(_codeGen->CreateICmpNE(resultPtr.getValue(),
                _codeGen->CreateIntToPtr(cg_size_t(1ul).getValue(), resultPtr.getValue()->getType())));
#endif
    }


    size_t i = 0;
    for (auto iu : required) {
        // the final value
        value_op_t sqlValue;

        if (iu->columnInformation->columnName.compare("tid") == 0) {

            //Add tid to the produced values
            llvm::Value *tidValue = tid.getValue();
            tidSqlValue = std::make_unique<LongInteger>(tidValue);
            values[iu] = tidSqlValue.get();
        } else {
            column_t & column = columns[i];

            ci_p_t ci = std::get<0>(column);

            llvm::Value *elemPtr;
            if (branchId != master_branch_id) {
                elemPtr = getBranchElemPtr(tid,column,resultPtr,ptrIsNotNull);
            } else {
                elemPtr = getMasterElemPtr(tid,column);
            }

            // calculate the SQL value pointer

            if (ci->type.nullable) {
                assert(ci->nullIndicatorType == ColumnInformation::NullIndicatorType::Column);
                // load null indicator
                cg_bool_t isNull = genNullIndicatorLoad(nullIndicatorTable, tid, cg_unsigned_t(ci->nullColumnIndex));
                SqlType notNullableType = toNotNullableTy(ci->type);
                auto loadedValue = Value::load(elemPtr, notNullableType);
                std::get<4>(column) = NullableValue::create(std::move(loadedValue), isNull);
            } else {
                // load the SQL value
                std::get<4>(column) = Value::load(elemPtr, ci->type);
            }

            // map the value to the according iu
            values[iu] = std::get<4>(column).get();

            i += 1;
        }
    }

    _parent->consume(values, *this);
    if (seesVersion) {
        seesVersion->EndIf();
    }
}
#else
void TableScan::produce(cg_tid_t tid) {
    iu_value_mapping_t values;

    // get null indicator column data
    auto & nullIndicatorTable = table.getNullIndicatorTable();
    iu_set_t required = getRequired();


    size_t i = 0;
    for (auto iu : required) {
        // the final value
        value_op_t sqlValue;

        if (iu->columnInformation->columnName.compare("tid") == 0) {
            //Add tid to the produced values
            llvm::Value *tidValue = tid.getValue();
            tidSqlValue = std::make_unique<LongInteger>(tidValue);
            values[iu] = tidSqlValue.get();
        } else {
            column_t & column = columns[i];

            ci_p_t ci = std::get<0>(column);

            llvm::Value *elemPtr = getMasterElemPtr(tid,column);

            // calculate the SQL value pointer

            if (ci->type.nullable) {
                assert(ci->nullIndicatorType == ColumnInformation::NullIndicatorType::Column);
                // load null indicator
                cg_bool_t isNull = genNullIndicatorLoad(nullIndicatorTable, tid, cg_unsigned_t(ci->nullColumnIndex));
                SqlType notNullableType = toNotNullableTy(ci->type);
                auto loadedValue = Value::load(elemPtr, notNullableType);
                std::get<4>(column) = NullableValue::create(std::move(loadedValue), isNull);
            } else {
                // load the SQL value
                std::get<4>(column) = Value::load(elemPtr, ci->type);
            }

            // map the value to the according iu
            values[iu] = std::get<4>(column).get();

            i += 1;
        }
    }

    _parent->consume(values, *this);
}
#endif

llvm::Value *TableScan::getMasterElemPtr(cg_tid_t &tid, column_t &column) {
#ifdef __APPLE__
    llvm::Value * elemPtr = _codeGen->CreateGEP(std::get<1>(column), std::get<2>(column), { cg_size_t(0ull), tid });
#else
    llvm::Value * elemPtr = _codeGen->CreateGEP(std::get<1>(column), std::get<2>(column), { cg_size_t(0ul), tid });
#endif
    return elemPtr;
}

llvm::Value *TableScan::getBranchElemPtr(cg_tid_t &tid, column_t &column, cg_voidptr_t &resultPtr, cg_bool_t &ptrIsNotNull) {
    IfGen check( _codeGen.getCurrentFunctionGen(), ptrIsNotNull, {{"elemPtr", cg_int_t(0)}} );
    {
        llvm::Value * valuePtr = tupleToElemPtr(resultPtr,column);
        check.setVar(0, valuePtr);
    }
    check.Else();
    {
        llvm::Value *elemPtr = getMasterElemPtr(tid,column);
        check.setVar(0, elemPtr);
    }
    check.EndIf();
    return check.getResult(0);
}

#if USE_DATA_VERSIONING
void TableScan::produceIndexProbe(const BranchIndex & index, uint64_t keyHash)
{
    auto & funcGen = _codeGen.getCurrentFunctionGen();
    auto & context = _codeGen.getLLVMContext();
    llvm::Type * i64Ty = llvm::Type::getInt64Ty(context);

    auto owned = std::make_unique<ProbeState>();
    ProbeState & state = *owned;
    _context.executionContext.acquireResource(std::move(owned));
    state.index = &index;
    state.keyHash = keyHash;

    auto & moduleGen = _codeGen.getCurrentModuleGen();
    llvm::Function * probe = LLVMCompat::getOrInsertFunction(moduleGen.getModule(), "index_probe",
            llvm::TypeBuilder<size_t (void *), false>::get(context));
    moduleGen.addFunctionMapping(probe, (void *) &index_probe);
    llvm::Function * candidatesFunc = LLVMCompat::getOrInsertFunction(moduleGen.getModule(), "index_candidates",
            llvm::TypeBuilder<void * (void *), false>::get(context));
    moduleGen.addFunctionMapping(candidatesFunc, (void *) &index_candidates);

    llvm::Value * statePtr = cg_ptr8_t::fromRawPointer(&state).getValue();
    llvm::Value * count = _codeGen->CreateCall(probe, {statePtr});
    llvm::Value * candidates = _codeGen->CreatePointerCast(_codeGen->CreateCall(candidatesFunc, {statePtr}), llvm::PointerType::getUnqual(i64Ty));

    LoopGen probeLoop(funcGen, _codeGen->CreateICmpULT(cg_size_t(0ul).getValue(), count), {{"candidate", cg_size_t(0ul)}});
    cg_size_t candidate(probeLoop.getLoopVar(0));
    {
        LoopBodyGen bodyGen(probeLoop);
        cg_tid_t tid(_codeGen->CreateLoad(i64Ty, _codeGen->CreateGEP(i64Ty, candidates, candidate.getValue())));
        IfGen visibilityCheck(isVisible(tid, branchId));
        {
            produce(tid, branchId);
        }
        visibilityCheck.EndIf();
    }
    cg_size_t nextCandidate = candidate + 1ul;
    probeLoop.loopDone(_codeGen->CreateICmpULT(nextCandidate.getValue(), count), {nextCandidate});
}

bool TableScan::isParallelPipeline()
{
    // selections pass tuples on; the result operators serialise the callbacks of the workers
    for (Operator * op = _parent; op != nullptr; op = op->getParent()) {
        if (dynamic_cast<Select *>(op) != nullptr) continue;
        return dynamic_cast<TupleStream *>(op) != nullptr || dynamic_cast<Print *>(op) != nullptr;
    }
    return false;
}

void TableScan::produceParallel()
{
    auto & context = _codeGen.getLLVMContext();
    auto owned = std::make_unique<ParallelState>();
    _parallel = owned.get();
    _parallel->table = &table;
    _parallel->branch = branchId;
    _parallel->tuples = table.size();
#if INTERVAL_VISIBILITY
    Database & db = table.getDatabase();
    _parallel->label = db._identL[db.getIdent(branchId)];
#endif
    _context.executionContext.acquireResource(std::move(owned));

    // the scan loop over a morsel and the operators above it form their own function
    llvm::Function * morsel;
    {
        FunctionGen funcGen(_codeGen.getCurrentModuleGen(), "table_scan_morsel",
                            llvm::TypeBuilder<void (size_t, size_t, size_t, size_t), false>::get(context));
        _context.parallelPipeline = true;
        llvm::Value * begin = funcGen.getArg(0);
        llvm::Value * end = funcGen.getArg(1);
        LoopGen scanLoop(funcGen, _codeGen->CreateICmpULT(begin, end), {{"index", begin}});
        cg_size_t tid(scanLoop.getLoopVar(0));
        {
            LoopBodyGen bodyGen(scanLoop);
            IfGen visibilityCheck(isVisible(tid, branchId));
            {
                produce(tid, branchId);
            }
            visibilityCheck.EndIf();
        }
        cg_size_t nextIndex = tid + 1ul;
        scanLoop.loopDone(_codeGen->CreateICmpULT(nextIndex.getValue(), end), {nextIndex});
        _context.parallelPipeline = false;
        morsel = funcGen.getFunction();
    }

    llvm::FunctionType * runTy = llvm::TypeBuilder<void (void *, void *, void *), false>::get(context);
    llvm::Function * run = LLVMCompat::getOrInsertFunction(getThreadLocalCodeGen().getCurrentModuleGen().getModule(), "table_scan_run", runTy);
    getThreadLocalCodeGen().getCurrentModuleGen().addFunctionMapping(run, (void *) &scan_run);
    _codeGen->CreateCall(run, {cg_ptr8_t::fromRawPointer(_parallel).getValue(),
            _codeGen->CreatePointerCast(morsel, LLVMCompat::getInt8PtrTy(context)), _codeGen.getCurrentFunctionGen().getArg(1)});
}
#endif

llvm::Value * TableScan::genChainWalkCall(cg_tid_t tid, branch_id_t branchId) {
#if USE_DATA_VERSIONING
    if (_parallel != nullptr) {
        llvm::FunctionType * funcTy = llvm::TypeBuilder<void * (size_t, void *), false>::get(_codeGen.getLLVMContext());
        llvm::Function * func = LLVMCompat::getOrInsertFunction(getThreadLocalCodeGen().getCurrentModuleGen().getModule(), "table_scan_latest_entry", funcTy);
        getThreadLocalCodeGen().getCurrentModuleGen().addFunctionMapping(func, (void *) &scan_latest_entry);
        return _codeGen->CreateCall(func, {tid.getValue(), cg_ptr8_t::fromRawPointer(_parallel).getValue()});
    }
#endif
    llvm::FunctionType * funcTy = llvm::TypeBuilder<void * (size_t, void *, branch_id_t, void *), false>::get(_codeGen.getLLVMContext());
    llvm::Function * func = LLVMCompat::getOrInsertFunction(getThreadLocalCodeGen().getCurrentModuleGen().getModule(), "get_latest_entry", funcTy);
    getThreadLocalCodeGen().getCurrentModuleGen().addFunctionMapping(func,(void *)&get_latest_entry);
    return _codeGen->CreateCall(func, {tid, cg_ptr8_t::fromRawPointer(&table), cg_u32_t(branchId), _codeGen.getCurrentFunctionGen().getArg(1)});
}

cg_voidptr_t TableScan::genGetLatestEntryCall(cg_tid_t tid, branch_id_t branchId) {
    std::unique_lock<std::mutex> scanLatch(table._scanLatch);
    auto versionMap = table._versionMaps.find(branchId);
    const bool hasVersionMap = (versionMap != table._versionMaps.end());
    scanLatch.unlock();
    if (hasVersionMap) {
        // the version map names the version of every tuple; it does not move while the query runs
        llvm::Type * i64Ty = llvm::Type::getInt64Ty(_codeGen.getLLVMContext());
        llvm::Value * versions = _codeGen->CreateIntToPtr(cg_size_t(reinterpret_cast<uintptr_t>(versionMap->second.data())).getValue(), llvm::PointerType::getUnqual(i64Ty));
        // main may redirect an entry from its in-place version to a copy meanwhile (InPlaceWrites)
        llvm::Value * version = LLVMCompat::makeMonotonic(_codeGen->CreateLoad(i64Ty, _codeGen->CreateGEP(i64Ty, versions, tid.getValue())), sizeof(uintptr_t));
        return cg_voidptr_t( _codeGen->CreateIntToPtr(version, LLVMCompat::getInt8PtrTy(_codeGen.getLLVMContext())) );
    }
#if INTERVAL_VISIBILITY
    if (!table.headsKnown()) {
        // rows were loaded without maintaining the head identifiers: always walk the chain
        return cg_voidptr_t( genChainWalkCall(tid, branchId) );
    }
    // Fast path compiled into the scan loop: if no version of another branch precedes the
    // in-place version and the reader's label lies in the label of its identifier, the tuple
    // is read in place (nullptr); otherwise the chain is walked in get_latest_entry.
    auto & context = _codeGen.getLLVMContext();
    llvm::Type * i8PtrTy = LLVMCompat::getInt8PtrTy(context);
    llvm::Type * i32Ty = llvm::Type::getInt32Ty(context);
    llvm::Type * i64Ty = llvm::Type::getInt64Ty(context);
    Database & db = table.getDatabase();

    // the labels and the head identifiers do not move while the query runs
    const uint64_t readerLabel = db._identL[db.getIdent(branchId)];

    llvm::Value * heads = _codeGen->CreateIntToPtr(cg_size_t(reinterpret_cast<uintptr_t>(table._head_ident.data())).getValue(), llvm::PointerType::getUnqual(i32Ty));
    // writers of other branches may store invalid_ident meanwhile (prepend_version)
    llvm::Value * head = LLVMCompat::makeMonotonic(_codeGen->CreateLoad(i32Ty, _codeGen->CreateGEP(i32Ty, heads, tid.getValue())), sizeof(ident_t));
    llvm::Value * headIsInPlace = _codeGen->CreateICmpNE(head, llvm::ConstantInt::get(i32Ty, invalid_ident));
    llvm::Value * ident = _codeGen->CreateZExt(_codeGen->CreateSelect(headIsInPlace, head, llvm::ConstantInt::get(i32Ty, 0)), i64Ty);
    llvm::Value * labelsL = _codeGen->CreateIntToPtr(cg_size_t(reinterpret_cast<uintptr_t>(db._identL.data())).getValue(), llvm::PointerType::getUnqual(i64Ty));
    llvm::Value * labelsR = _codeGen->CreateIntToPtr(cg_size_t(reinterpret_cast<uintptr_t>(db._identR.data())).getValue(), llvm::PointerType::getUnqual(i64Ty));
    llvm::Value * l = _codeGen->CreateLoad(i64Ty, _codeGen->CreateGEP(i64Ty, labelsL, ident));
    llvm::Value * r = _codeGen->CreateLoad(i64Ty, _codeGen->CreateGEP(i64Ty, labelsR, ident));
    llvm::Value * reader = cg_size_t(readerLabel).getValue();
    llvm::Value * inPlace = _codeGen->CreateAnd(headIsInPlace,
                            _codeGen->CreateAnd(_codeGen->CreateICmpULE(l, reader), _codeGen->CreateICmpULE(reader, r)));

    IfGen slowPath(_codeGen.getCurrentFunctionGen(), _codeGen->CreateNot(inPlace), {{"latest", cg_size_t(0ul)}});
    {
        llvm::Value * result = genChainWalkCall(tid, branchId);
        slowPath.setVar(0, _codeGen->CreatePtrToInt(result, i64Ty));
    }
    slowPath.EndIf();
    return cg_voidptr_t( _codeGen->CreateIntToPtr(slowPath.getResult(0), i8PtrTy) );
#else
    return cg_voidptr_t( genChainWalkCall(tid, branchId) );
#endif
}

cg_bool_t TableScan::nullPointerCheck(cg_voidptr_t &ptr) {
#ifdef __APPLE__
    return cg_bool_t(cg_size_t(_codeGen->CreatePtrToInt(ptr, _codeGen->getIntNTy(64))) != cg_size_t(0ull));
#else
    return cg_bool_t(cg_size_t(_codeGen->CreatePtrToInt(ptr, _codeGen->getIntNTy(64))) != cg_size_t(0ul));
#endif
}

llvm::Value *TableScan::tupleToElemPtr(cg_voidptr_t &ptr, column_t &column) {
    llvm::Type * tupleTy = Sql::SqlTuple::getType(table.getTupleType());
    llvm::Type * tuplePtrTy = llvm::PointerType::getUnqual(tupleTy);
    llvm::Value * tuplePtr = _codeGen->CreatePointerCast(ptr, tuplePtrTy);

    return _codeGen->CreateStructGEP(tupleTy, tuplePtr, std::get<3>(column));
}

cg_bool_t TableScan::isVisible(cg_tid_t tid, cg_branch_id_t branchId)
{
    // the parameter is the scanned branch, which is known when the scan is compiled
    return genMembershipTest(table.getMembership(), tid, branchId.getValue(), this->branchId);
}

} // end namespace Physical
} // end namespace Algebra
