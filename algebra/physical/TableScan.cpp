//
// Created by josef on 31.12.16.
//

#include "algebra/physical/TableScan.hpp"

//#include <llvm/IR/TypeBuilder.h>
//#include "foundations/version_management.hpp"
#include <unordered_map>

#include "codegen/PhiNode.hpp"
#include "sql/SqlUtils.hpp"

using namespace Sql;

namespace Algebra {
namespace Physical {

TableScan::TableScan(const logical_operator_t & logicalOperator, Table & table) :
        NullaryOperator(std::move(logicalOperator)),
        table(table)
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

        columns.emplace_back(ci, columnTy, columnPtr);
    }
}

TableScan::~TableScan()
{ }

void TableScan::produce()
{
    //Functions::genPrintfCall("%s\n",_codeGen.getCurrentFunctionGen().getArg(1));

    //llvm::FunctionType * funcTy = llvm::TypeBuilder<void (), false>::get(_codeGen.getLLVMContext());
    //lvm::CallInst * result = _codeGen.CreateCall(&sampleFunction, funcTy, {_codeGen.getCurrentFunctionGen().getArg(1)});

    auto & funcGen = _codeGen.getCurrentFunctionGen();

    size_t tableSize = table.size();
    if (tableSize < 1) {
        return; // nothing to produce
    }

    // iterate over all tuples
#ifdef __APPLE__
    LoopGen scanLoop(funcGen, {{"index", cg_size_t(0ull)}});
#else
    LoopGen scanLoop(funcGen, {{"index", cg_size_t(0ul)}});
#endif
    cg_size_t tid(scanLoop.getLoopVar(0));
    {
        LoopBodyGen bodyGen(scanLoop);

        auto branchId = _context.executionContext.branchId;
        IfGen visibilityCheck(isVisible(tid, branchId));
        {
            produce(tid);
        }
        visibilityCheck.EndIf();
    }
    cg_size_t nextIndex = tid + 1ul;
    scanLoop.loopDone(nextIndex < tableSize, {nextIndex});
}

void TableScan::produce(cg_tid_t tid) {
    std::unordered_map<const InformationUnit*, std::unique_ptr<Sql::Value>> values;

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
            sqlValue = std::make_unique<LongInteger>(tidValue);
            values[iu] = std::move(sqlValue);

        } else {
            column_t & column = columns[i];
            ci_p_t ci = std::get<0>(column);

            // calculate the SQL value pointer
#ifdef __APPLE__
            llvm::Value * elemPtr = _codeGen->CreateGEP(std::get<1>(column), std::get<2>(column), { cg_size_t(0ull), tid });
#else
            llvm::Value * elemPtr = _codeGen->CreateGEP(std::get<1>(column), std::get<2>(column), { cg_size_t(0ul), tid });
#endif

            if (ci->type.nullable) {
                assert(ci->nullIndicatorType == ColumnInformation::NullIndicatorType::Column);
                // load null indicator
                cg_bool_t isNull = genNullIndicatorLoad(nullIndicatorTable, tid, cg_unsigned_t(ci->nullColumnIndex));
                SqlType notNullableType = toNotNullableTy(ci->type);
                auto loadedValue = Value::load(elemPtr, notNullableType);
                sqlValue = NullableValue::create(std::move(loadedValue), isNull);
            } else {
                // load the SQL value
                sqlValue = Value::load(elemPtr, ci->type);
            }

            // map the value to the according iu
            values[iu] = std::move(sqlValue);

            i += 1;
        }
    }

    iu_value_mapping_t returnValues;
    for (auto& mapping : values) {
        returnValues.emplace(mapping.first,mapping.second.get());
    }

    _parent->consume(returnValues, *this);
}

cg_bool_t TableScan::isVisible(cg_tid_t tid, cg_branch_id_t branchId)
{
    auto & branchBitmap = table.getBranchBitmap();
    return isVisibleInBranch(branchBitmap, tid, branchId);
}

} // end namespace Physical
} // end namespace Algebra
