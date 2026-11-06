#include "algebra/physical/Insert.hpp"
#include "sql/SqlUtils.hpp"
#include "sql/SqlValues.hpp"
#include "foundations/version_management.hpp"
#include "codegen/LLVMCompat.hpp"

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
            _context.executionContext.affectedRows = tuples.size();
#if USE_DATA_VERSIONING
            genInsertCall((void *)&insert_tuple_with_branchId);
#else
            genInsertCall((void *)&insert_tuple_without_versioning);
#endif
        }

        void Insert::genInsertCall(void *funcPtr) {
            llvm::FunctionType * funcTy = llvm::TypeBuilder<tid_t (void *, void *, void *, branch_id_t), false>::get(_codeGen.getLLVMContext());
            llvm::Function * func = LLVMCompat::getOrInsertFunction(getThreadLocalCodeGen().getCurrentModuleGen().getModule(), "insert_tuple_with_binding", funcTy);
            getThreadLocalCodeGen().getCurrentModuleGen().addFunctionMapping(func,funcPtr);
            if (tuples.empty()) return;

            // a loop over the tuples, which outlive this operator in the execution context: one call
            // per tuple in straight-line code would take time quadratic in their number to compile
            auto array = std::make_unique<PointerArrayResource>();
            array->pointers.assign(tuples.begin(), tuples.end());
            void ** pointers = array->pointers.data();
            _context.executionContext.acquireResource(std::move(array));

            auto & llvmContext = _codeGen.getLLVMContext();
            llvm::Type * pointerTy = LLVMCompat::getInt8PtrTy(llvmContext);
            llvm::Value * base = _codeGen->CreatePointerCast(cg_ptr8_t::fromRawPointer(pointers), llvm::PointerType::getUnqual(pointerTy));
            cg_size_t count(tuples.size());
            auto & funcGen = _codeGen.getCurrentFunctionGen();
            LoopGen loop(funcGen, {{"tuple", cg_size_t(0ul)}});
            cg_size_t index(loop.getLoopVar(0));
            {
                LoopBodyGen body(loop);
                llvm::Value * tuple = _codeGen->CreateLoad(pointerTy, _codeGen->CreateGEP(pointerTy, base, index.getValue()));
                _codeGen->CreateCall(func, {tuple, cg_ptr8_t::fromRawPointer(&table), funcGen.getArg(1), cg_u32_t(branchId)});
            }
            cg_size_t next = index + 1ul;
            loop.loopDone(_codeGen->CreateICmpULT(next.getValue(), count.getValue()), {next});
        }

    } // end namespace Physical
} // end namespace Algebra

