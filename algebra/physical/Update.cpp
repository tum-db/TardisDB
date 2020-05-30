//
// Created by josef on 03.01.17.
//

#include <llvm/IR/TypeBuilder.h>

#include "algebra/physical/Update.hpp"
#include "sql/SqlUtils.hpp"
#include "sql/SqlValues.hpp"
#include "sql/SqlTuple.hpp"
#include "foundations/version_management.hpp"

#define Versioning

using namespace Sql;

namespace Algebra {
    namespace Physical {

        Update::Update(const logical_operator_t & logicalOperator, std::unique_ptr<Operator> input, Table & table,
                std::vector<std::pair<iu_p_t,std::string>> &updateIUs) :
                UnaryOperator(std::move(logicalOperator), std::move(input)) , table(table),
                _updateIUs(std::move(updateIUs))
        {
            // collect all information which is necessary to access the columns
            for (auto &iuPairs : _updateIUs) {
                auto ci = getColumnInformation(iuPairs.first);
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

                std::unique_ptr<Sql::Value> sqlValue = nullptr;
                if (iuPairs.second.compare("") != 0) {
                    sqlValue = Sql::Value::castString(iuPairs.second,storedSqlType);
                }

                size_t tableSize = table.size();
                llvm::Type * elemTy = toLLVMTy(storedSqlType);
                llvm::Type * columnTy = llvm::ArrayType::get(elemTy, tableSize);
                llvm::Value * columnPtr = createPointerValue(ci->column->front(), columnTy);
                size_t columnIndex = 0;
                for (int i = 0; i<table.getColumnCount(); i++) {
                    if (table.getColumnNames()[i].compare(ci->columnName) == 0) {
                        columnIndex = i;
                        break;
                    }
                }

                columns.emplace_back(ci, columnTy, columnPtr, columnIndex, std::move(sqlValue));
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

#ifdef Versioning
        void storeStringInTextFormat(char* destination, char* source, char length) {
            destination[0] = length;
            std::memcpy((void*)&destination[1],source,length);
        }

        std::unique_ptr<Native::Sql::Value> sqlValueToNativeSqlValue(Sql::Value *original) {
            if (original == nullptr) return nullptr;

            std::unique_ptr<Native::Sql::Value> returnValue = nullptr;

            auto & codeGen = getThreadLocalCodeGen();
            auto & context = codeGen.getLLVMContext();

            llvm::FunctionType * funcTy;
            llvm::CallInst * result;

            switch (original->type.typeID) {
                case Sql::SqlType::TypeID::UnknownID:
                    break;
                case Sql::SqlType::TypeID::BoolID:
                    returnValue = std::make_unique<Native::Sql::Bool>();
                    original->store(cg_ptr8_t::fromRawPointer(&((Native::Sql::Bool*) returnValue.get())->value));
                    break;
                case Sql::SqlType::TypeID::IntegerID:
                    returnValue = std::make_unique<Native::Sql::Integer>();
                    original->store(cg_ptr8_t::fromRawPointer(&((Native::Sql::Integer*) returnValue.get())->value));
                    break;
                case Sql::SqlType::TypeID::VarcharID:
                    returnValue = std::make_unique<Native::Sql::Text>();
                    funcTy = llvm::TypeBuilder<void * (void * , void *, uint8_t), false>::get(codeGen.getLLVMContext());
                    result = codeGen.CreateCall(&storeStringInTextFormat, funcTy, {cg_ptr8_t::fromRawPointer(&((Native::Sql::Text*) returnValue.get())->value), cg_voidptr_t(((Sql::Varchar*) original)->getLLVMValue()), cg_u8_t(((Sql::Varchar*) original)->getLength())});
                    break;
                case Sql::SqlType::TypeID::NumericID:
                    returnValue = std::make_unique<Native::Sql::Numeric>();
                    original->store(cg_ptr8_t::fromRawPointer(&((Native::Sql::Numeric*) returnValue.get())->value));
                    break;
                case Sql::SqlType::TypeID::DateID:
                    returnValue = std::make_unique<Native::Sql::Date>();
                    original->store(cg_ptr8_t::fromRawPointer(&((Native::Sql::Date*) returnValue.get())->value));
                    break;
                case Sql::SqlType::TypeID::TimestampID:
                    returnValue = std::make_unique<Native::Sql::Timestamp>();
                    original->store(cg_ptr8_t::fromRawPointer(&((Native::Sql::Timestamp*) returnValue.get())->value));
                    break;
                case Sql::SqlType::TypeID::TextID:
                    returnValue = std::make_unique<Native::Sql::Text>();
                    original->store(cg_ptr8_t::fromRawPointer(&((Native::Sql::Text*) returnValue.get())->value));
                    break;
                default:
                    throw InvalidOperationException("unknown type");
            }
            return std::move(returnValue);
        }
#endif

        void Update::consume(const iu_value_mapping_t & values, const Operator & src)
        {
            // Get the tid of the consumed tuple
            cg_size_t tid;
            for (auto &iuPair : _updateIUs) {
                if (iuPair.first->columnInformation->columnName.compare("tid") == 0) {
                    tid = cg_size_t(values.at(iuPair.first)->getLLVMValue());
                    break;
                }
            }


#ifdef Versioning
            // Collect all values needed for the row tuple in correct order
            std::vector<Sql::Value*> tupleValues;
            std::sort(columns.begin(), columns.end(), sortColumns);
            for (auto &column : columns) {
                for (auto &iuPair : _updateIUs) {
                    if (iuPair.first->columnInformation->columnName.compare(std::get<0>(column)->columnName) == 0) {
                        // If tuple value needs to be updated take the value offered by the operator; Else take the value pushed up in the tree
                        std::unique_ptr<Sql::Value> &updateValue = std::get<4>(column);
                        if (updateValue != nullptr) {
                            tupleValues.emplace_back(updateValue.get());
                        } else {
                            tupleValues.emplace_back(values.at(iuPair.first));
                        }
                        break;
                    }
                }
            }

            // Convert the Sql::Value to Native::Sql::Value
            std::vector<std::unique_ptr<Native::Sql::Value>> nativeValues;
            for (auto &value : tupleValues) {
                nativeValues.emplace_back(sqlValueToNativeSqlValue(value));
            }

            // Create a Native::Sql::Tuple needed by the update_tuple function in version_management.hpp
            Native::Sql::SqlTuple* nativetuple = new Native::Sql::SqlTuple(std::move(nativeValues));

            // Call the update_tuple function in version_management.hpp
            llvm::FunctionType * funcUpdateTupleTy = llvm::TypeBuilder<void * (size_t, void * , void *, void *), false>::get(_codeGen.getLLVMContext());
            llvm::CallInst * result = _codeGen.CreateCall(&update_tuple, funcUpdateTupleTy, {tid, cg_ptr8_t::fromRawPointer(nativetuple), cg_ptr8_t::fromRawPointer(&table), _codeGen.getCurrentFunctionGen().getArg(1)});

#else

            for (auto &column : columns) {
                Sql::Value *sqlValue = std::get<4>(column).get();
                if (sqlValue == nullptr) continue;

                // calculate the SQL value pointer
#ifdef __APPLE__
                llvm::Value * elemPtr = _codeGen->CreateGEP(std::get<1>(column), std::get<2>(column), { cg_size_t(0ull), tid });
#else
                llvm::Value * elemPtr = _codeGen->CreateGEP(std::get<1>(column), std::get<2>(column), { cg_size_t(0ul), tid });
#endif
                // map the value to the according iu

                // Store the new value at desired position
                sqlValue->store(elemPtr);
            }
#endif

            // increment tuple counter
            cg_size_t prevCnt(_codeGen->CreateLoad(tupleCountPtr));
            _codeGen->CreateStore(prevCnt + 1ul, tupleCountPtr);
        }

    } // end namespace Physical
} // end namespace Algebra

