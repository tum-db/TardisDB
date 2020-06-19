//
// Created by Blum Thomas on 2020-06-16.
//

#include <llvm/IR/TypeBuilder.h>

#include "ValueTranslator.hpp"

void ValueTranslator::storeStringInTextFormat(char* destination, char* source, char length) {
    destination[0] = length;
    std::memcpy((void*)&destination[1],source,length);
}

std::unique_ptr<Native::Sql::Value> ValueTranslator::sqlValueToNativeSqlValue(Sql::Value *original) {
    if (original == nullptr) return nullptr;

    std::unique_ptr<Native::Sql::Value> returnValue = nullptr;

    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();

    llvm::FunctionType * funcTy;
    llvm::Function * func;
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
            func = llvm::cast<llvm::Function>( getThreadLocalCodeGen().getCurrentModuleGen().getModule().getOrInsertFunction("storeStringInTextFormat", funcTy) );
            getThreadLocalCodeGen().getCurrentModuleGen().addFunctionMapping(func,(void *)&storeStringInTextFormat);
            result = codeGen->CreateCall(func, {cg_ptr8_t::fromRawPointer(&((Native::Sql::Text*) returnValue.get())->value), cg_voidptr_t(((Sql::Varchar*) original)->getLLVMValue()), cg_u8_t(((Sql::Varchar*) original)->getLength())});
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
            return nullptr;
    }
    return std::move(returnValue);
}