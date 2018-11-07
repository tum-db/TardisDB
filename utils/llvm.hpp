#pragma once

#include <llvm/IR/Type.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Value.h>

inline llvm::Type * getPointeeType(llvm::Value * ptr)
{
    return llvm::cast<llvm::PointerType>(ptr->getType()->getScalarType())->getElementType();
}
