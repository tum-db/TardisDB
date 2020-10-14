#include "algebra/physical/Insert.hpp"
#include "sql/SqlUtils.hpp"
#include "sql/SqlValues.hpp"
#include "foundations/version_management.hpp"
#include <llvm/IR/TypeBuilder.h>

#include <iostream>

using namespace Sql;

namespace Algebra {
    namespace Physical {

        Insert::Insert(const logical_operator_t & logicalOperator, Table & table, Native::Sql::SqlTuple *tuple, QueryContext &context) :
                NullaryOperator(std::move(logicalOperator)) , table(table), tuple(tuple), context(context)
        {

        }

        Insert::~Insert()
        { }

#if !USE_DATA_VERSIONING
        tid_t insert_tuple_without_versioning(Native::Sql::SqlTuple & tuple, Table & table, QueryContext & ctx) {
            tid_t tid;

            // store tuple
            table.addRow(0);
            size_t column_idx = 0;
            for (auto & value : tuple.values) {
                void * ptr = const_cast<void *>(table.getColumn(column_idx).back());
                value->store(ptr);
                column_idx += 1;
            }

            return tid;
        }
#endif

        void Insert::produce()
        {
            //Native::Sql::SqlTuple *tuplePtr = tuple.get();
            /*llvm::FunctionType * funcTy = llvm::TypeBuilder<int (void*,void*,void*), false>::get(_codeGen.getLLVMContext());
            llvm::CallInst * result = _codeGen.CreateCall(&insert_tuple, funcTy, {tuplePtr,tablePtr,contextPtr});*/
            llvm::FunctionType * funcTy = llvm::TypeBuilder<void (void *, void *, void *), false>::get(_codeGen.getLLVMContext());
            llvm::Function * func = llvm::cast<llvm::Function>( getThreadLocalCodeGen().getCurrentModuleGen().getModule().getOrInsertFunction("insert_tuple", funcTy) );
#if USE_DATA_VERSIONING
            getThreadLocalCodeGen().getCurrentModuleGen().addFunctionMapping(func,(void *)&insert_tuple);
#else
            getThreadLocalCodeGen().getCurrentModuleGen().addFunctionMapping(func,(void *)&insert_tuple_without_versioning);
#endif
            llvm::CallInst * result = _codeGen->CreateCall(func, {cg_ptr8_t::fromRawPointer(tuple), cg_ptr8_t::fromRawPointer(&table), _codeGen.getCurrentFunctionGen().getArg(1)});
        }

    } // end namespace Physical
} // end namespace Algebra

