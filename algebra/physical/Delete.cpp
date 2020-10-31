#include <llvm/IR/TypeBuilder.h>

#include "algebra/physical/Delete.hpp"
#include "sql/SqlUtils.hpp"
#include "sql/SqlValues.hpp"
#include "foundations/version_management.hpp"

using namespace Sql;

namespace Algebra {
    namespace Physical {

        Delete::Delete(const logical_operator_t & logicalOperator, std::unique_ptr<Operator> input, iu_p_t &tidIU, Table & table, QueryContext &queryContext)  :
                UnaryOperator(std::move(logicalOperator), std::move(input), queryContext), table(table), tidIU(std::move(tidIU)) { }

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

#if !USE_DATA_VERSIONING
        void delete_tuple_without_versioning(tid_t tid, Table & table, QueryContext & ctx) {
            table.removeRow(tid);
        }
#endif

        void Delete::consume(const iu_value_mapping_t & values, const Operator & src)
        {
            cg_size_t tid;
            for (auto iu : getRequired()) {
                if (iu->columnInformation->columnName.compare("tid") == 0) {
                    tid = cg_size_t(values.at(iu)->getLLVMValue());
                    break;
                }
            }

            // Call the delete_tuple function in version_management.hpp
            llvm::FunctionType * funcDeleteTupleTy = llvm::TypeBuilder<void (size_t, void *, void *), false>::get(_codeGen.getLLVMContext());
            llvm::Function * func = llvm::cast<llvm::Function>( getThreadLocalCodeGen().getCurrentModuleGen().getModule().getOrInsertFunction("delete_tuple", funcDeleteTupleTy) );
#if USE_DATA_VERSIONING
            getThreadLocalCodeGen().getCurrentModuleGen().addFunctionMapping(func,(void *)&delete_tuple);
#else
            getThreadLocalCodeGen().getCurrentModuleGen().addFunctionMapping(func,(void *)&delete_tuple_without_versioning);
#endif
            llvm::CallInst * result = _codeGen->CreateCall(func, {tid, cg_ptr8_t::fromRawPointer(&table), _codeGen.getCurrentFunctionGen().getArg(1)});

            // increment tuple counter
            cg_size_t prevCnt(_codeGen->CreateLoad(tupleCountPtr));
            _codeGen->CreateStore(prevCnt + 1ul, tupleCountPtr);
        }

    } // end namespace Physical
} // end namespace Algebra

