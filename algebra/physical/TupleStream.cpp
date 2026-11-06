//
// Created by Blum Thomas on 2020-06-16.
//

#include "codegen/LLVMCompat.hpp"

#include "TupleStream.hpp"
#include "sql/SqlUtils.hpp"
#include "sql/ValueTranslator.hpp"
#include "foundations/Parallel.hpp"

using namespace Sql;

namespace Algebra {
    namespace Physical {

        TupleStream::TupleStream(const logical_operator_t & logicalOperator, std::unique_ptr<Operator> input, QueryContext &queryContext) :
                UnaryOperator(std::move(logicalOperator), std::move(input), queryContext)
        {
            // in the order of the projection
            for (auto &iu : static_cast<const Logical::Result &>(_logicalOperator).selection) {
                if (iu->iuType == InformationUnit::Type::ColumnRef && getColumnInformation(iu)->columnName.compare("tid") == 0) {
                    continue;
                }

                columns.emplace_back(iu);
            }
        }

        TupleStream::~TupleStream()
        { }

        void TupleStream::produce()
        {
            // the counter may be incremented by the workers of a parallel pipeline
            auto counter = std::make_unique<CounterResource>();
            tupleCount = &counter->value;
            _context.executionContext.acquireResource(std::move(counter));
            _codeGen->CreateStore(cg_size_t(0ul), _codeGen->CreatePointerCast(cg_ptr8_t::fromRawPointer(tupleCount), llvm::PointerType::getUnqual(cg_size_t::getType())));

            child->produce();
        }

        void TupleStream::consume(const iu_value_mapping_t & values, const Operator & src)
        {
            // workers share the native tuple and the callback
            llvm::FunctionType * lockTy = llvm::TypeBuilder<void (), false>::get(_codeGen.getLLVMContext());
            if (_context.parallelPipeline) {
                _codeGen.CreateCall(&Parallel::lockResults, lockTy);
            }

            std::vector<Sql::Value*> tupleValues;
            for (auto &column : columns) {
                tupleValues.emplace_back(values.at(std::get<0>(column)));
            }

            // Convert the Sql::Value to Native::Sql::Value
            std::vector<std::unique_ptr<Native::Sql::Value>> nativeValues;
            for (auto &value : tupleValues) {
                nativeValues.emplace_back(ValueTranslator::sqlValueToNativeSqlValue(value));
            }

            // Create a Native::Sql::Tuple needed by the update_tuple function in version_management.hpp
            Native::Sql::SqlTuple* nativetuple = new Native::Sql::SqlTuple(std::move(nativeValues));

            // Call the update_tuple function in version_management.hpp
            genCallbackCall(nativetuple);

            // increment tuple counter
            llvm::Value * countPtr = _codeGen->CreatePointerCast(cg_ptr8_t::fromRawPointer(tupleCount), llvm::PointerType::getUnqual(cg_size_t::getType()));
            cg_size_t prevCnt(_codeGen->CreateLoad(cg_size_t::getType(), countPtr));
            _codeGen->CreateStore(prevCnt + 1ul, countPtr);

            if (_context.parallelPipeline) {
                _codeGen.CreateCall(&Parallel::unlockResults, lockTy);
            }
        }

        void TupleStream::genCallbackCall(Native::Sql::SqlTuple* nativetuple) {
            if (_context.tupleSink) {
                llvm::FunctionType * sinkTy = llvm::TypeBuilder<void (void *, void *), false>::get(_codeGen.getLLVMContext());
                llvm::Function * sink = LLVMCompat::getOrInsertFunction(getThreadLocalCodeGen().getCurrentModuleGen().getModule(), "emitToSink", sinkTy);
                getThreadLocalCodeGen().getCurrentModuleGen().addFunctionMapping(sink, (void *) &QueryContext::emitToSink);
                _codeGen->CreateCall(sink, {cg_ptr8_t::fromRawPointer(&_context), cg_ptr8_t::fromRawPointer(nativetuple)});
                return;
            }
            llvm::FunctionType * funcUpdateTupleTy = llvm::TypeBuilder<void (void *), false>::get(_codeGen.getLLVMContext());
            llvm::Function * func = LLVMCompat::getOrInsertFunction(getThreadLocalCodeGen().getCurrentModuleGen().getModule(), "callHandler", funcUpdateTupleTy);
            _codeGen->CreateCall(func, {cg_ptr8_t::fromRawPointer(nativetuple)});
        }

    } // end namespace Physical
} // end namespace Algebra