//
// Created by josef on 31.12.16.
//

#include "algebra/physical/TableScan.hpp"

#include <llvm/IR/TypeBuilder.h>
#include "foundations/version_management.hpp"
#include <unordered_map>

#include "sql/SqlTuple.hpp"

using namespace Sql;

namespace Algebra {
namespace Physical {

TableScan::TableScan(const logical_operator_t & logicalOperator, Table & table, std::string *alias) :
        NullaryOperator(std::move(logicalOperator)),
        table(table),
        alias(alias)
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

    // iterate over all tuples
#ifdef __APPLE__
    LoopGen scanLoop(funcGen, {{"index", cg_size_t(0ull)}});
#else
    LoopGen scanLoop(funcGen, {{"index", cg_size_t(0ul)}});
#endif
    cg_size_t tid(scanLoop.getLoopVar(0));
    {
        LoopBodyGen bodyGen(scanLoop);

        auto branchId = _context.executionContext.branchIds[*alias];
        IfGen visibilityCheck(isVisible(tid, branchId));
        {
            produce(tid, branchId);
        }
        visibilityCheck.EndIf();
    }
    cg_size_t nextIndex = tid + 1ul;
    scanLoop.loopDone(nextIndex < tableSize, {nextIndex});

    //Free the memory pointed by the alias string pointer
    Functions::genFreeCall(cg_voidptr_t::fromRawPointer(alias));
}



void TableScan::produce(cg_tid_t tid, branch_id_t branchId) {
    iu_value_mapping_t values;

    // get null indicator column data
    auto & nullIndicatorTable = table.getNullIndicatorTable();
    iu_set_t required = getRequired();

    cg_voidptr_t resultPtr;
    cg_bool_t ptrIsNotNull(false);
    if (branchId != master_branch_id) {
        llvm::FunctionType * funcTy = llvm::TypeBuilder<void * (size_t, void * , void* , void *), false>::get(_codeGen.getLLVMContext());
        llvm::Function * func = llvm::cast<llvm::Function>( getThreadLocalCodeGen().getCurrentModuleGen().getModule().getOrInsertFunction("get_latest_entry", funcTy) );
        getThreadLocalCodeGen().getCurrentModuleGen().addFunctionMapping(func,(void *)&get_latest_entry);
        llvm::CallInst * result = _codeGen->CreateCall(func, {tid, cg_ptr8_t::fromRawPointer(&table), cg_ptr8_t::fromRawPointer(alias), _codeGen.getCurrentFunctionGen().getArg(1)});
        resultPtr = cg_voidptr_t( llvm::cast<llvm::Value>(result) );
#ifdef __APPLE__
        ptrIsNotNull = cg_bool_t(cg_size_t(_codeGen->CreatePtrToInt(resultPtr, _codeGen->getIntNTy(64))) != cg_size_t(0ull));
#else
        ptrIsNotNull = cg_bool_t(cg_size_t(_codeGen->CreatePtrToInt(resultPtr, _codeGen->getIntNTy(64))) != cg_size_t(0ul));
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
}

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
        llvm::Type * tupleTy = Sql::SqlTuple::getType(table.getTupleType());
        llvm::Type * tuplePtrTy = llvm::PointerType::getUnqual(tupleTy);
        llvm::Value * tuplePtr = _codeGen->CreatePointerCast(resultPtr, tuplePtrTy);

        cg_size_t index_gen = cg_size_t(std::get<3>(column));
        llvm::Value * valuePtr = _codeGen->CreateStructGEP(tupleTy, tuplePtr, std::get<3>(column));
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

cg_bool_t TableScan::isVisible(cg_tid_t tid, cg_branch_id_t branchId)
{
    auto & branchBitmap = table.getBranchBitmap();
    return isVisibleInBranch(branchBitmap, tid, branchId);
}

} // end namespace Physical
} // end namespace Algebra
