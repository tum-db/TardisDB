//
// Created by josef on 31.12.16.
//

#include "algebra/physical/TableScan.hpp"

#include <map>

#include "codegen/PhiNode.hpp"
#include "sql/SqlUtils.hpp"

using namespace Sql;

namespace Algebra {
namespace Physical {

TableScan::TableScan(QueryContext & context, const iu_set_t & required, Table & table) :
        NullaryOperator(context, required),
        table(table)
{ }

TableScan::~TableScan()
{ }

#ifndef USE_INTERNAL_NULL_INDICATOR
// external

//#define USE_NEW_EXTRACT
#ifndef USE_NEW_EXTRACT

void TableScan::produce()
{
    auto & funcGen = codeGen.getCurrentFunctionGen();

    size_t tableSize = table.size();
    if (tableSize < 1) {
        return; // nothing to produce
    }

    typedef std::tuple<ci_p_t, llvm::Type *, llvm::Value *> column_t;

    // collect all information which is necessary to access the columns
    iu_set_t required = getRequired();
    std::vector<column_t> columns;
    for (auto iu : required) {
        auto ci = getColumnInformation(iu);

        SqlType storedSqlType;
        if (ci->type.nullable) {
            // the null indicator will be stored in the NullIndicatorTable
            storedSqlType = toNotNullableTy( ci->type );
        } else {
            storedSqlType = ci->type;
        }

        llvm::Type * elemTy = toLLVMTy(storedSqlType);
        llvm::Type * columnTy = llvm::ArrayType::get(elemTy, tableSize);
        llvm::Value * columnPtr = createPointerValue(ci->column->front(), columnTy);

        columns.emplace_back(ci, columnTy, columnPtr);
    }

    // get null indicator column data
    auto & nullIndicatorTable = table.getNullIndicatorTable();

    // iterate over all tuples
    LoopGen scanLoop(funcGen, {{"index", cg_size_t(0ul)}});
    cg_size_t tid(scanLoop.getLoopVar(0));
    {
        LoopBodyGen bodyGen(scanLoop);

        iu_value_mapping_t values;
        std::vector<value_op_t> sqlValues;

        size_t i = 0;
        for (auto iu : required) {
            column_t & column = columns[i];
            ci_p_t ci = std::get<0>(column);

            // calculate the SQL value pointer
            llvm::Value * elemPtr = codeGen->CreateGEP(
                std::get<1>(column), std::get<2>(column), { cg_size_t(0ul), tid });

            // the final value
            value_op_t sqlValue;
            if (ci->type.nullable) {
                assert(ci->nullIndicatorType == ColumnInformation::NullIndicatorType::Column);

                // load null indicator
                cg_bool_t isNull = genNullIndicatorLoad(nullIndicatorTable, tid, cg_unsigned_t(ci->nullColumnIndex));
//Functions::genPrintfCall("loaded isNull %d\n", isNull);

                SqlType notNullableType = toNotNullableTy(ci->type);

                // TODO compare performance
#define LOAD_NULLABLE
#ifdef LOAD_NULLABLE
                auto loadedValue = Value::load(elemPtr, notNullableType);
                sqlValue = NullableValue::create(std::move(loadedValue), isNull);
#else
                auto nullValue = NullableValue::getNull(ci->type);
                PhiNode<Value> resultNode(*nullValue, "resultNode");
                IfGen notNullCheck(!isNull);
                {
                    // load the SQL value
                    auto loadedValue = Value::load(elemPtr, notNullableType);
                    auto nullableValue = NullableValue::create(std::move(loadedValue), cg_bool_t(false));
                    resultNode = *nullableValue;
                }
                notNullCheck.EndIf();

                // resolve the PHI Node
                sqlValue = resultNode.get();
#endif // LOAD_NULLABLE

            } else {
                // load the SQL value
                sqlValue = Value::load(elemPtr, ci->type);
            }

            // map the value to the according iu
            values[iu] = sqlValue.get();
            sqlValues.push_back( std::move(sqlValue) );

            i += 1;
        }

        parent->consume(values, *this);
    }
    cg_size_t nextIndex = tid + 1ul;
    scanLoop.loopDone(nextIndex < tableSize, {nextIndex});
}

#else // USE_NEW_EXTRACT

void TableScan::produce()
{
    auto & funcGen = codeGen.getCurrentFunctionGen();

    size_t tableSize = table.size();
    if (tableSize < 1) {
        return; // nothing to produce
    }

    struct ScanInfo {
        ci_p_t ci;
        llvm::Type * columnTy;
        llvm::Value * columnPtr;

        // null indiciator related:
        unsigned section;
        cg_i8_t nullIndicatorMask;
    };

    // get null indicator column data
    auto & nullIndicatorTable = table.getNullIndicatorTable();
    cg_ptr8_t nullIndicatorTablePtr = cg_ptr8_t::fromRawPointer(nullIndicatorTable.data());
    unsigned sectionCount = nullIndicatorTable.getRowSize();
    cg_size_t bytesPerNullIndicatorRow(sectionCount);
    std::map<unsigned, PhiNode<llvm::Value>> sectionPointers;

    // collect all information which is necessary to access the columns
    iu_set_t required = getRequired();
    std::vector<ScanInfo> columns(required.size());
    size_t i = 0;
    for (auto iu : required) {
        auto ci = getColumnInformation(iu);

        SqlType storedSqlType;
        if (ci->type.nullable) {
            // the null indicator will be stored in the NullIndicatorTable
            storedSqlType = toNotNullableTy( ci->type );
        } else {
            storedSqlType = ci->type;
        }

        ScanInfo & info = columns[i];
        i += 1;

        info.ci = ci;
        llvm::Type * elemTy = toLLVMTy(storedSqlType);
        info.columnTy = llvm::ArrayType::get(elemTy, tableSize);
        info.columnPtr = createPointerValue(ci->column->front(), info.columnTy);

        // null indicator:
        if (ci->type.nullable) {
            cg_unsigned_t nullColumnIndex(ci->nullColumnIndex);
            unsigned section = ci->nullColumnIndex % 8;
            // add a new section?
            auto it = sectionPointers.find(section);
            if (it == sectionPointers.end()) {
                // section index within the row
                cg_size_t sectionIndex( codeGen->CreateLShr(nullColumnIndex, 3) );
                cg_ptr8_t sectionPtr = nullIndicatorTablePtr + sectionIndex.llvmValue;
                sectionPointers.emplace(section, PhiNode<llvm::Value>(
                    sectionPtr.llvmValue, "nullIndicator_" + std::to_string(ci->nullColumnIndex)));
            }

            // create mask:
            cg_unsigned_t indicatorIndex = nullColumnIndex & cg_unsigned_t(7);
            cg_i8_t indicatorIndexInt8(indicatorIndex);
            cg_i8_t indicatorMask = cg_i8_t(1) << indicatorIndexInt8;

            info.section = section;
            info.nullIndicatorMask = indicatorMask;
        }
    }

    // iterate over all tuples
    LoopGen scanLoop(funcGen, {{"index", cg_size_t(0ul)}});
    cg_size_t tid(scanLoop.getLoopVar(0));
    {
        LoopBodyGen bodyGen(scanLoop);

        // load null indicators
        std::map<unsigned, llvm::Value *> sectionValues;
        for (auto & entry : sectionPointers) {
            llvm::Value * sectionPtr = entry.second.get();
            cg_i8_t sectionData( codeGen->CreateLoad(cg_i8_t::getType(), sectionPtr) );
            sectionValues[entry.first] = sectionData.llvmValue;
        }

        iu_value_mapping_t values;
        std::vector<value_op_t> sqlValues;

        size_t j = 0;
        for (auto iu : required) {
            ScanInfo & scanInfo = columns[j];
            j += 1;

            ci_p_t ci = scanInfo.ci;

            // calculate the SQL value pointer
            llvm::Value * elemPtr = codeGen->CreateGEP(
                scanInfo.columnTy, scanInfo.columnPtr, { cg_size_t(0ul), tid });

            // the final value
            value_op_t sqlValue;
            if (ci->type.nullable) {
                assert(ci->nullIndicatorType == ColumnInformation::NullIndicatorType::Column);

                // load null indicator
                cg_i8_t sectionData( sectionValues.at(scanInfo.section) );
                cg_i8_t masked = sectionData & scanInfo.nullIndicatorMask;
                cg_bool_t isNull( codeGen->CreateICmpNE( masked, codeGen->getInt8(0) ) );

//Functions::genPrintfCall("sectionPtr %p, loaded isNull %d\n", sectionPtr, isNull);

                SqlType notNullableType = toNotNullableTy(ci->type);

                // TODO compare performance
//#define LOAD_NULLABLE
#ifdef LOAD_NULLABLE
                auto loadedValue = Value::load(elemPtr, notNullableType);
                sqlValue = NullableValue::create(std::move(loadedValue), isNull);
#else
                auto nullValue = NullableValue::getNull(ci->type);
                PhiNode<Value> resultNode(*nullValue, "resultNode");
                IfGen notNullCheck(!isNull);
                {
                    // load the SQL value
                    auto loadedValue = Value::load(elemPtr, notNullableType);
                    auto nullableValue = NullableValue::create(std::move(loadedValue), cg_bool_t(false));
                    resultNode = *nullableValue;
                }
                notNullCheck.EndIf();

                // resolve the PHI Node
                sqlValue = resultNode.get();
#endif // LOAD_NULLABLE

            } else {
                // load the SQL value
                sqlValue = Value::load(elemPtr, ci->type);
            }

            // map the value to the according iu
            values[iu] = sqlValue.get();
            sqlValues.push_back( std::move(sqlValue) );
        }

        parent->consume(values, *this);
    }
    // advance null indicator pointers
    for (auto & entry : sectionPointers) {
        PhiNode<llvm::Value> & phiNode = entry.second;
        llvm::Value * current = phiNode.get();
        cg_ptr8_t next = cg_ptr8_t(current) + bytesPerNullIndicatorRow;
        phiNode.addIncoming(next);
    }

    cg_size_t nextIndex = tid + 1ul;
    scanLoop.loopDone(nextIndex < tableSize, {nextIndex});
}

#endif // USE_NEW_EXTRACT

#else // USE_INTERNAL_NULL_INDICATOR
// internal
void TableScan::produce()
{
    auto & funcGen = codeGen.getCurrentFunctionGen();

    size_t tableSize = table.size();
    if (tableSize < 1) {
        return; // nothing to produce
    }

    typedef std::tuple<ci_p_t, llvm::Type *, llvm::Value *> column_t;

    // collect all information which is necessary to access the columns
    iu_set_t required = getRequired();
    std::vector<column_t> columns;
    for (auto iu : required) {
        auto ci = getColumnInformation(iu);

        SqlType storedSqlType = ci->type;

        llvm::Type * elemTy = toLLVMTy(storedSqlType);
        llvm::Type * columnTy = llvm::ArrayType::get(elemTy, tableSize);
        llvm::Value * columnPtr = createPointerValue(ci->column->front(), columnTy);

        columns.emplace_back(ci, columnTy, columnPtr);
    }

    // iterate over all tuples
    LoopGen scanLoop(funcGen, {{"index", cg_size_t(0ul)}});
    cg_size_t tid(scanLoop.getLoopVar(0));
    {
        LoopBodyGen bodyGen(scanLoop);

        iu_value_mapping_t values;
        std::vector<value_op_t> sqlValues;

        size_t i = 0;
        for (auto iu : required) {
            column_t & column = columns[i];
            ci_p_t ci = std::get<0>(column);

            // calculate the SQL value pointer
            llvm::Value * elemPtr = codeGen->CreateGEP(
                std::get<1>(column), std::get<2>(column), { cg_size_t(0ul), tid });

            // the final value
            value_op_t sqlValue;
            if (ci->type.nullable) {
                assert(ci->nullIndicatorType == ColumnInformation::NullIndicatorType::Embedded);

                sqlValue = NullableValue::load(elemPtr, ci->type);
            } else {
                // load the SQL value
                sqlValue = Value::load(elemPtr, ci->type);
            }

            // map the value to the according iu
            values[iu] = sqlValue.get();
            sqlValues.push_back( std::move(sqlValue) );

            i += 1;
        }

        parent->consume(values, *this);
    }
    cg_size_t nextIndex = tid + 1ul;
    scanLoop.loopDone(nextIndex < tableSize, {nextIndex});
}

#endif // USE_INTERNAL_NULL_INDICATOR

} // end namespace Physical
} // end namespace Algebra
