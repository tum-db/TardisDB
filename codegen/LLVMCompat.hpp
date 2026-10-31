#pragma once

// Compatibility with LLVM 7 (the original target) and newer LLVM versions (tested: 18).
//
// LLVM 9 removed llvm::TypeBuilder, and LLVM 15 made pointers opaque. TardisDB states element types
// explicitly at loads and GEPs, which works with both; the remaining differences are wrapped here.

#include <llvm/Config/llvm-config.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>

#include <cstdint>
#include <type_traits>

#if LLVM_VERSION_MAJOR < 9

#include <llvm/IR/TypeBuilder.h>

#else

namespace llvm {

/// Maps C++ types to LLVM types, as llvm::TypeBuilder<T, false> of LLVM 7 did: integers by their
/// size, pointers to i8 for void, and function types including varargs.
template<typename T, bool cross = false>
class TypeBuilder {
public:
    static Type * get(LLVMContext & context) {
        static_assert(std::is_integral<T>::value || std::is_enum<T>::value, "unsupported type");
        return IntegerType::get(context, 8 * sizeof(T));
    }
};

template<bool cross> class TypeBuilder<void, cross> {
public:
    static Type * get(LLVMContext & context) { return Type::getVoidTy(context); }
};

template<bool cross> class TypeBuilder<bool, cross> {
public:
    static Type * get(LLVMContext & context) { return Type::getInt1Ty(context); }
};

template<typename T, bool cross> class TypeBuilder<const T, cross> : public TypeBuilder<T, cross> { };
template<typename T, bool cross> class TypeBuilder<volatile T, cross> : public TypeBuilder<T, cross> { };
template<typename T, bool cross> class TypeBuilder<const volatile T, cross> : public TypeBuilder<T, cross> { };

template<typename T, bool cross> class TypeBuilder<T *, cross> {
public:
    static PointerType * get(LLVMContext & context) {
        Type * element = std::is_void<typename std::remove_cv<T>::type>::value
                ? static_cast<Type *>(Type::getInt8Ty(context)) : TypeBuilder<T, cross>::get(context);
        return PointerType::getUnqual(element);
    }
};

template<typename R, typename... Args, bool cross> class TypeBuilder<R (Args...), cross> {
public:
    static FunctionType * get(LLVMContext & context) {
        return FunctionType::get(TypeBuilder<R, cross>::get(context), {TypeBuilder<Args, cross>::get(context)...}, false);
    }
};

template<typename R, typename... Args, bool cross> class TypeBuilder<R (Args..., ...), cross> {
public:
    static FunctionType * get(LLVMContext & context) {
        return FunctionType::get(TypeBuilder<R, cross>::get(context), {TypeBuilder<Args, cross>::get(context)...}, true);
    }
};

} // end namespace llvm

#endif

namespace LLVMCompat {

/// \returns The declaration of the function, as Module::getOrInsertFunction of LLVM 7 did
inline llvm::Function * getOrInsertFunction(llvm::Module & module, llvm::StringRef name, llvm::FunctionType * type)
{
#if LLVM_VERSION_MAJOR < 9
    return llvm::cast<llvm::Function>(module.getOrInsertFunction(name, type));
#else
    return llvm::cast<llvm::Function>(module.getOrInsertFunction(name, type).getCallee());
#endif
}

inline llvm::Function * getOrInsertFunction(llvm::Module & module, llvm::StringRef name, llvm::FunctionType * type,
                                           llvm::AttributeList attributes)
{
#if LLVM_VERSION_MAJOR < 9
    return llvm::cast<llvm::Function>(module.getOrInsertFunction(name, type, attributes));
#else
    return llvm::cast<llvm::Function>(module.getOrInsertFunction(name, type, attributes).getCallee());
#endif
}

/// \returns The type of a pointer to i8
inline llvm::PointerType * getInt8PtrTy(llvm::LLVMContext & context)
{
    return llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(context));
}

/// \returns The type of a pointer to an integer of the given width
inline llvm::PointerType * getIntNPtrTy(llvm::LLVMContext & context, unsigned bits)
{
    return llvm::PointerType::getUnqual(llvm::IntegerType::get(context, bits));
}

/// Optimisation level of the code generator: none
#if LLVM_VERSION_MAJOR < 18
constexpr auto codeGenOptNone = llvm::CodeGenOpt::None;
#else
constexpr auto codeGenOptNone = llvm::CodeGenOptLevel::None;
#endif

/// memcpy without alignment guarantees
inline llvm::CallInst * createMemCpy(llvm::IRBuilder<> & builder, llvm::Value * dst, llvm::Value * src, llvm::Value * size)
{
#if LLVM_VERSION_MAJOR < 10
    return builder.CreateMemCpy(dst, 1, src, 1, size);
#else
    return builder.CreateMemCpy(dst, llvm::MaybeAlign(1), src, llvm::MaybeAlign(1), size);
#endif
}

inline llvm::CallInst * createMemCpy(llvm::IRBuilder<> & builder, llvm::Value * dst, llvm::Value * src, uint64_t size)
{
    return createMemCpy(builder, dst, src, builder.getInt64(size));
}

/// memset without alignment guarantees
inline llvm::CallInst * createMemSet(llvm::IRBuilder<> & builder, llvm::Value * dst, llvm::Value * value, llvm::Value * size)
{
#if LLVM_VERSION_MAJOR < 10
    return builder.CreateMemSet(dst, value, size, 1);
#else
    return builder.CreateMemSet(dst, value, size, llvm::MaybeAlign(1));
#endif
}

} // end namespace LLVMCompat
