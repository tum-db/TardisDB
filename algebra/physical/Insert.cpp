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

        void Insert::produce()
        {
            //Native::Sql::SqlTuple *tuplePtr = tuple.get();
            /*llvm::FunctionType * funcTy = llvm::TypeBuilder<int (void*,void*,void*), false>::get(_codeGen.getLLVMContext());
            llvm::CallInst * result = _codeGen.CreateCall(&insert_tuple, funcTy, {tuplePtr,tablePtr,contextPtr});*/
            llvm::FunctionType * funcTy = llvm::TypeBuilder<void (void *, void *, void *), false>::get(_codeGen.getLLVMContext());
            llvm::CallInst * result = _codeGen.CreateCall(&insert_tuple, funcTy, {cg_ptr8_t::fromRawPointer(tuple), cg_ptr8_t::fromRawPointer(&table), _codeGen.getCurrentFunctionGen().getArg(1)});
            std::cout << "Insert\n";
        }

    } // end namespace Physical
} // end namespace Algebra

