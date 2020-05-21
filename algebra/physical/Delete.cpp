#include "algebra/physical/Delete.hpp"
#include "sql/SqlUtils.hpp"
#include "sql/SqlValues.hpp"

using namespace Sql;

namespace Algebra {
    namespace Physical {

        Delete::Delete(const logical_operator_t & logicalOperator, std::unique_ptr<Operator> input, Table & table)  :
                UnaryOperator(std::move(logicalOperator), std::move(input)), table(table) {}

        Delete::~Delete()
        { }

        void Delete::produce()
        {
            tupleCountPtr = _codeGen->CreateAlloca(cg_size_t::getType());
#ifdef __APPLE__
            _codeGen->CreateStore(cg_size_t(0ull), tupleCountPtr);
#else
            _codeGen->CreateStore(cg_size_t(0ul), tupleCountPtr);
#endif

            child->produce();

            cg_size_t tupleCnt(_codeGen->CreateLoad(tupleCountPtr));

            Functions::genPrintfCall("Deleted %lu tuples\n", tupleCnt);
        }

        void Delete::consume(const iu_value_mapping_t & values, const Operator & src)
        {
            cg_size_t tid;
            for (auto iu : getRequired()) {
                if (iu->columnInformation->columnName.compare("tid") == 0) {
                    tid = cg_size_t(values.at(iu)->getLLVMValue());
                    break;
                }
            }

            Functions::genPrintfCall("Delete TID %lu\n",tid);

            // increment tuple counter
            cg_size_t prevCnt(_codeGen->CreateLoad(tupleCountPtr));
            _codeGen->CreateStore(prevCnt + 1ul, tupleCountPtr);
        }

    } // end namespace Physical
} // end namespace Algebra

