#include "algebra/physical/Insert.hpp"
#include "sql/SqlUtils.hpp"
#include "sql/SqlValues.hpp"
#include "foundations/version_management.hpp"
#include <llvm/IR/TypeBuilder.h>

#include <iostream>

using namespace Sql;

namespace Algebra {
    namespace Physical {

        Insert::Insert(const logical_operator_t & logicalOperator, Table & table, std::vector <Native::Sql::SqlTuple *> tuples, QueryContext &context, branch_id_t branchId) :
                NullaryOperator(std::move(logicalOperator),context) , table(table), tuples(move(tuples)), context(context), branchId(branchId)
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
#if USE_DATA_VERSIONING
            genInsertCall((void *)&insert_tuple_with_branchId);
#else
            genInsertCall((void *)&insert_tuple_without_versioning);
#endif
        }

        void Insert::genInsertCall(void *funcPtr) {
            llvm::FunctionType * funcTy = llvm::TypeBuilder<void (void *, void *, void *), false>::get(_codeGen.getLLVMContext());
            llvm::Function * func = llvm::cast<llvm::Function>( getThreadLocalCodeGen().getCurrentModuleGen().getModule().getOrInsertFunction("insert_tuple_with_binding", funcTy) );
            getThreadLocalCodeGen().getCurrentModuleGen().addFunctionMapping(func,funcPtr);
            // call insert for every single tuple
            for(auto tuple: tuples)
                _codeGen->CreateCall(func, {cg_ptr8_t::fromRawPointer(tuple), cg_ptr8_t::fromRawPointer(&table), _codeGen.getCurrentFunctionGen().getArg(1), cg_u32_t(branchId)});
        }

    } // end namespace Physical
} // end namespace Algebra

