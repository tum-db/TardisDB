//
// Created by Blum Thomas on 2020-06-16.
//

#include "codegen/LLVMCompat.hpp"

#include "ValueTranslator.hpp"

void ValueTranslator::storeStringInTextFormat(Native::Sql::Varchar* destination, char* source, size_t length) {
    destination->value = source;
    destination->len = length;
}

void ValueTranslator::storeCharInTextFormat(Native::Sql::Varchar* destination, uint8_t character) {
    destination->single = static_cast<char>(character);
    destination->value = &destination->single;
    destination->len = 1;
}

std::unique_ptr<Native::Sql::Value> ValueTranslator::sqlValueToNativeSqlValue(Sql::Value *original) {
    if (original == nullptr) return nullptr;

    std::unique_ptr<Native::Sql::Value> returnValue = nullptr;

    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();

    llvm::FunctionType * funcTy;
    llvm::Function * func;
    llvm::CallInst * result;

    // a nullable value: its value, and the null indicator, which the generated code sets
    if (Sql::isNullable(*original)) {
        auto & nullable = static_cast<Sql::NullableValue &>(*original);
        std::unique_ptr<Native::Sql::Value> inner = sqlValueToNativeSqlValue(&nullable.getValue());
        if (inner == nullptr) return nullptr;
        returnValue = Native::Sql::NullableValue::create(std::move(inner), false);
        bool * indicator = static_cast<Native::Sql::NullableValue *>(returnValue.get())->getNullIndicatorPtr();
        llvm::Type * i8Ty = llvm::Type::getInt8Ty(context);
        codeGen->CreateStore(codeGen->CreateZExt(nullable.isNull().llvmValue, i8Ty),
                             codeGen->CreateIntToPtr(cg_size_t(reinterpret_cast<uintptr_t>(indicator)).getValue(), llvm::PointerType::getUnqual(i8Ty)));
        return returnValue;
    }

    switch (original->type.typeID) {
        case Sql::SqlType::TypeID::UnknownID:
            return nullptr;
        case Sql::SqlType::TypeID::BoolID:
            returnValue = std::make_unique<Native::Sql::Bool>(original->type);
            original->store(cg_ptr8_t::fromRawPointer(&((Native::Sql::Bool*) returnValue.get())->value));
            break;
        case Sql::SqlType::TypeID::IntegerID:
            returnValue = std::make_unique<Native::Sql::Integer>(original->type);
            original->store(cg_ptr8_t::fromRawPointer(&((Native::Sql::Integer*) returnValue.get())->value));
            break;
        case Sql::SqlType::TypeID::VarcharID:
            returnValue = std::make_unique<Native::Sql::Varchar>(original->type);
            funcTy = llvm::TypeBuilder<void * (void * , void *, size_t), false>::get(codeGen.getLLVMContext());
            func = LLVMCompat::getOrInsertFunction(getThreadLocalCodeGen().getCurrentModuleGen().getModule(), "storeStringInTextFormat", funcTy);
            getThreadLocalCodeGen().getCurrentModuleGen().addFunctionMapping(func,(void *)&storeStringInTextFormat);
            // the length indicator has 8, 16 or 32 bits by the capacity
            result = codeGen->CreateCall(func, {cg_ptr8_t::fromRawPointer((Native::Sql::Varchar*) returnValue.get()), cg_voidptr_t(((Sql::Varchar*) original)->getLLVMValue()), cg_size_t(codeGen->CreateZExtOrTrunc(((Sql::Varchar*) original)->getLength(), cg_size_t::getType()))});
            break;
        case Sql::SqlType::TypeID::CharID:
            // a native Varchar points to the characters, as for varchar
            returnValue = std::make_unique<Native::Sql::Varchar>(original->type);
            if (original->type.length == 1) {
                // a char(1) is the character itself
                funcTy = llvm::TypeBuilder<void (void *, uint8_t), false>::get(codeGen.getLLVMContext());
                func = LLVMCompat::getOrInsertFunction(getThreadLocalCodeGen().getCurrentModuleGen().getModule(), "storeCharInTextFormat", funcTy);
                getThreadLocalCodeGen().getCurrentModuleGen().addFunctionMapping(func,(void *)&storeCharInTextFormat);
                codeGen->CreateCall(func, {cg_ptr8_t::fromRawPointer((Native::Sql::Varchar*) returnValue.get()), cg_u8_t(original->getLLVMValue())});
                break;
            }
            funcTy = llvm::TypeBuilder<void * (void * , void *, size_t), false>::get(codeGen.getLLVMContext());
            func = LLVMCompat::getOrInsertFunction(getThreadLocalCodeGen().getCurrentModuleGen().getModule(), "storeStringInTextFormat", funcTy);
            getThreadLocalCodeGen().getCurrentModuleGen().addFunctionMapping(func,(void *)&storeStringInTextFormat);
            result = codeGen->CreateCall(func, {cg_ptr8_t::fromRawPointer((Native::Sql::Varchar*) returnValue.get()), cg_voidptr_t(((Sql::Char*) original)->getLLVMValue()), cg_size_t(codeGen->CreateZExtOrTrunc(((Sql::Char*) original)->getLength(), cg_size_t::getType()))});
            break;
        case Sql::SqlType::TypeID::LongIntegerID:
            // there is no native LongInteger; a Numeric holds the 64 bits, the type says what they are
            returnValue = std::make_unique<Native::Sql::Numeric>(original->type);
            original->store(cg_ptr8_t::fromRawPointer(&((Native::Sql::Numeric*) returnValue.get())->value));
            break;
        case Sql::SqlType::TypeID::NumericID:
            returnValue = std::make_unique<Native::Sql::Numeric>(original->type);
            original->store(cg_ptr8_t::fromRawPointer(&((Native::Sql::Numeric*) returnValue.get())->value));
            break;
        case Sql::SqlType::TypeID::DateID:
            returnValue = std::make_unique<Native::Sql::Date>(original->type);
            original->store(cg_ptr8_t::fromRawPointer(&((Native::Sql::Date*) returnValue.get())->value));
            break;
        case Sql::SqlType::TypeID::TimestampID:
            returnValue = std::make_unique<Native::Sql::Timestamp>(original->type);
            original->store(cg_ptr8_t::fromRawPointer(&((Native::Sql::Timestamp*) returnValue.get())->value));
            break;
        case Sql::SqlType::TypeID::TextID:
            returnValue = std::make_unique<Native::Sql::Text>(original->type);
            original->store(cg_ptr8_t::fromRawPointer((void*)((Native::Sql::Text*) returnValue.get())->value.data()));
            break;
        default:
            return nullptr;
    }

    return std::move(returnValue);
}