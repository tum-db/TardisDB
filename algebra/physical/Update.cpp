//
// Created by josef on 03.01.17.
//

#include "algebra/physical/Update.hpp"
#include "sql/SqlUtils.hpp"
#include "sql/SqlValues.hpp"

using namespace Sql;

namespace Algebra {
    namespace Physical {

        Update::Update(const logical_operator_t & logicalOperator, std::unique_ptr<Operator> input, Table & table,
                std::vector<iu_p_t> &updateIUs, std::vector<std::unique_ptr<Sql::Value>> &updateValues) :
                UnaryOperator(std::move(logicalOperator), std::move(input)) , table(table),
                _updateIUs(std::move(updateIUs)), _updateValues(std::move(updateValues))
        {
            // collect all information which is necessary to access the columns
            for (auto iu : _updateIUs) {
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

        Update::~Update()
        { }

        void Update::produce()
        {
            tupleCountPtr = _codeGen->CreateAlloca(cg_size_t::getType());
#ifdef __APPLE__
            _codeGen->CreateStore(cg_size_t(0ull), tupleCountPtr);
#else
            _codeGen->CreateStore(cg_size_t(0ul), tupleCountPtr);
#endif

            child->produce();

            cg_size_t tupleCnt(_codeGen->CreateLoad(tupleCountPtr));

            Functions::genPrintfCall("Updated %lu tuples\n", tupleCnt);
        }

        void Update::consume(const iu_value_mapping_t & values, const Operator & src)
        {
            cg_size_t tid;
            for (auto iu : getRequired()) {
                if (iu->columnInformation->columnName.compare("tid") == 0) {
                    tid = cg_size_t(values.at(iu)->getLLVMValue());
                    break;
                }
            }

            auto& updateIUs = _logicalOperator.getRequired();

            size_t i = 0;
            for (iu_p_t iu : updateIUs) {
                if (iu->columnInformation->columnName.compare("tid") == 0) {
                    continue;
                }
                column_t & column = columns[i];
                ci_p_t ci = std::get<0>(column);

                // calculate the SQL value pointer
#ifdef __APPLE__
                llvm::Value * elemPtr = _codeGen->CreateGEP(std::get<1>(column), std::get<2>(column), { cg_size_t(0ull), tid });
#else
                llvm::Value * elemPtr = _codeGen->CreateGEP(std::get<1>(column), std::get<2>(column), { cg_size_t(0ul), tid });
#endif
                // map the value to the according iu

                // Store the new value at desired position
                _updateValues[i]->store(elemPtr);

                i += 1;
            }

            // increment tuple counter
            cg_size_t prevCnt(_codeGen->CreateLoad(tupleCountPtr));
            _codeGen->CreateStore(prevCnt + 1ul, tupleCountPtr);
        }

    } // end namespace Physical
} // end namespace Algebra

