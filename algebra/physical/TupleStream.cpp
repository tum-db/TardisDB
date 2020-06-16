//
// Created by Blum Thomas on 2020-06-16.
//

#include <llvm/IR/TypeBuilder.h>

#include "TupleStream.hpp"
#include "sql/SqlUtils.hpp"
#include "sql/ValueTranslator.hpp"

using namespace Sql;

namespace Algebra {
    namespace Physical {

        TupleStream::TupleStream(const logical_operator_t & logicalOperator, std::unique_ptr<Operator> input) :
                UnaryOperator(std::move(logicalOperator), std::move(input))
        {
            for (auto &iu : _logicalOperator.getRequired()) {
                auto ci = getColumnInformation(iu);
                if (ci->columnName.compare("tid") == 0) {
                    continue;
                }

                columns.emplace_back(iu);
            }

            _handler = [](Native::Sql::SqlTuple &tuple) {
                std::cout << "hier\n";
                std::cout << Native::Sql::toString(tuple) << " works!\n";
            };
        }

        TupleStream::~TupleStream()
        { }

        void TupleStream::produce()
        {
            tupleCountPtr = _codeGen->CreateAlloca(cg_size_t::getType());
#ifdef __APPLE__
            _codeGen->CreateStore(cg_size_t(0ull), tupleCountPtr);
#else
            _codeGen->CreateStore(cg_size_t(0ul), tupleCountPtr);
#endif

            child->produce();

            cg_size_t tupleCnt(_codeGen->CreateLoad(tupleCountPtr));
#ifdef __APPLE__
            IfGen check(tupleCnt == cg_size_t(0ull));
#else
            IfGen check(tupleCnt == cg_size_t(0ul));
#endif
            {
                Functions::genPrintfCall("Empty result set\n");
            }
            check.Else();
            {
                Functions::genPrintfCall("Produced %lu tuples\n", tupleCnt);
            }
            check.EndIf();
        }

        void callTupleHandler(Native::Sql::SqlTuple &tuple) {
            std::cout << "hier\n";
        }

        void TupleStream::consume(const iu_value_mapping_t & values, const Operator & src)
        {
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
            llvm::FunctionType * funcUpdateTupleTy = llvm::TypeBuilder<void (void *), false>::get(_codeGen.getLLVMContext());
            llvm::CallInst * result = _codeGen.CreateCall(&TupleStream::callHandler, funcUpdateTupleTy, {cg_ptr8_t::fromRawPointer(&_handler), cg_ptr8_t::fromRawPointer(nativetuple)});

            // increment tuple counter
            cg_size_t prevCnt(_codeGen->CreateLoad(tupleCountPtr));
            _codeGen->CreateStore(prevCnt + 1ul, tupleCountPtr);
        }

    } // end namespace Physical
} // end namespace Algebra