#pragma once

#include <llvm/IR/Type.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Value.h>

#include <llvm/Config/llvm-config.h>

#if LLVM_VERSION_MAJOR < 15
/// \note Only with typed pointers; state element types explicitly instead
inline llvm::Type * getPointeeType(llvm::Value * ptr)
{
    return llvm::cast<llvm::PointerType>(ptr->getType()->getScalarType())->getElementType();
}
#endif
