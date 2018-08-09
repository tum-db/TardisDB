//
// Created by josef on 26.11.16.
//

#ifndef LLVM_PROTOTYPE_CODEGEN_H
#define LLVM_PROTOTYPE_CODEGEN_H

#include <utility>

#include <llvm/IR/IRBuilder.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>

#include "TypeCache.hpp"

class CodeGen;
class ModuleGen;
class FunctionGen;

//-----------------------------------------------------------------------------

CodeGen & getThreadLocalCodeGen();

//-----------------------------------------------------------------------------

/// CodeGen provides some basic functionality as well
/// as access to the underlying IRBuilder by using the "->" operator
class CodeGen {
public:
    CodeGen();
    ~CodeGen();

    // disable copy constructor and assignment operator
    CodeGen & operator =(const CodeGen &) = delete;
    CodeGen(const CodeGen &) = delete;

    llvm::IRBuilder<> & getIRBuilder();

    llvm::LLVMContext & getLLVMContext();

    TypeCache & getTypeCache();

    /// \brief Generates a call to the function pointer f
    template<typename Func>
    llvm::CallInst * CreateCall(
            Func f,
            llvm::FunctionType * FTy,
            const llvm::Twine & Name = "",
            llvm::MDNode * FPMathTag = nullptr);

    /// \brief Generates a call to the function pointer f
    template<typename Func>
    llvm::CallInst * CreateCall(
            Func f,
            llvm::FunctionType * FTy,
            llvm::ArrayRef<llvm::Value *> Args,
            const llvm::Twine & Name = "",
            llvm::MDNode * FPMathTag = nullptr);

    void setCurrentModuleGen(ModuleGen & moduleGen);
    ModuleGen & getCurrentModuleGen();
    void finalizeModuleGen();
    llvm::Module & getCurrentModule();
    bool hasModuleGen() const { return currentModuleGen != nullptr; }

    void setCurrentFunctionGen(FunctionGen & functionGen);
    FunctionGen & getCurrentFunctionGen();
    void finalizeFunctionGen();
    llvm::Function & getCurrentFunction();
    bool hasFunctionGen() const { return currentFunctionGen != nullptr; }

    /// \brief Set the BasicBlock that should be executed next
    void setNextBlock(llvm::BasicBlock * nextBB);

    /// \brief Create a branch to the next BasicBlock
    void leaveBlock(llvm::BasicBlock * afterBB);

    llvm::IRBuilder<> * operator->();

private:
    llvm::LLVMContext context;
    std::vector<llvm::BasicBlock *> nextBBVec;
    ModuleGen * currentModuleGen;
    FunctionGen * currentFunctionGen;
    llvm::IRBuilder<> builder;
    TypeCache typeCache;
};

//-----------------------------------------------------------------------------

class ModuleGen {
public:
    ModuleGen(const std::string & moduleName);
    ~ModuleGen();

    CodeGen & getCodeGen();

    std::unique_ptr<llvm::Module> finalizeModule();

    llvm::LLVMContext & getContext();

    llvm::Module & getModule();

    llvm::DataLayout & getDataLayout();

    void addFunctionMapping(llvm::Function * func, void * funcPtr);
    void applyMapping(llvm::ExecutionEngine * ee);

private:
    llvm::LLVMContext & context;
    CodeGen & codeGen; // cache to avoid thread_local access overhead

    std::unique_ptr<llvm::Module> module;
    std::unique_ptr<llvm::DataLayout> dataLayout;

    std::vector<std::pair<llvm::Function *, void *>> functionMapping;
};

//-----------------------------------------------------------------------------

class FunctionGen {
public:
    FunctionGen(ModuleGen & moduleGen, llvm::StringRef name,
                llvm::FunctionType * functionTypeType);
    FunctionGen(ModuleGen & moduleGen, llvm::StringRef name,
                llvm::FunctionType * functionTypeType, llvm::AttributeSet attributeList);
    ~FunctionGen();

    llvm::Function * getFunction();

    /// \brief Returns the corresponding function argument
    llvm::Argument * getArg(size_t i);

    /// \brief Used to set the return value. Do not use this for void functions.
    void setReturnValue(llvm::Value * value);

    ModuleGen & getModuleGen();

    CodeGen & getCodeGen();

    llvm::LLVMContext & getContext();

private:
    ModuleGen & moduleGen;
    llvm::Function * function;
    llvm::Value * returnValue;
};

//-----------------------------------------------------------------------------

class IfGen {
public:
    typedef std::vector<std::pair<std::string, llvm::Value *>> initial_values_vec_t;
    typedef std::vector<std::pair<llvm::Value *, llvm::BasicBlock *>> incoming_vars_vec_t;
    typedef std::vector<std::pair<std::string, incoming_vars_vec_t>> if_vars_vec_t;

    enum State { StateThen, StateElseIf, StateElse, StateDone };

    IfGen(llvm::Value * condV);

    /// \deprecated remove
    IfGen(FunctionGen & functionGen, llvm::Value * condV);

    /// \deprecated remove
    IfGen(FunctionGen & functionGen, llvm::Value * condV, const initial_values_vec_t & initialValues);

    ~IfGen();

    // disable copy constructor and assignment operator
    IfGen & operator =(const IfGen &) = delete;
    IfGen(const IfGen &) = delete;

    CodeGen & getCodeGen();

    llvm::LLVMContext & getContext();

    llvm::BasicBlock * Else();

    llvm::BasicBlock * ElseIf(llvm::Value * condV);

    void EndIf();

    /// \brief Returns the value of the corresponding PHI-Node
    llvm::Value * getResult(size_t i);

    /// \brief Set the value of the corresponding PHI-Node
    void setVar(size_t i, llvm::Value * value);

private:
    FunctionGen & functionGen;

    State state;
    std::vector<llvm::BasicBlock *> blocks;
    llvm::BasicBlock * prevBB;
    llvm::BasicBlock * mergeBB;

    if_vars_vec_t ifVars;
};

//-----------------------------------------------------------------------------

class LoopGen {
public:
    typedef std::vector<std::pair<std::string, llvm::Value *>> loop_var_vec_t;
    typedef std::vector<llvm::Value *> loop_val_vec_t;

    LoopGen(FunctionGen & functionGen, const loop_var_vec_t & initialValues);
    LoopGen(FunctionGen & functionGen, llvm::Value * condV, const loop_var_vec_t & initialValues);
    ~LoopGen();

    // disable copy constructor and assignment operator
    LoopGen & operator =(const LoopGen &) = delete;
    LoopGen(const LoopGen &) = delete;

    CodeGen & getCodeGen();

    llvm::LLVMContext &  getContext();

    /// \brief Close the loop body
    void loopBodyEnd();

    /// \brief Evaluate the loop continuation condition
    void loopDone(llvm::Value * condV, const loop_val_vec_t & currentValues);

    /// \brief Break
    void loopBreak();

    /// \brief Continue
    void loopContinue();

    /// \brief Returns the value of the corresponding PHI-Node
    llvm::Value * getLoopVar(size_t i);

private:
    FunctionGen & functionGen;

    llvm::BasicBlock * loopBB;
    llvm::BasicBlock * loopContBB;
    llvm::BasicBlock * afterBB;

    std::vector<llvm::PHINode *> loopVars;
};

//-----------------------------------------------------------------------------

/// LoopBodyGen is a RAII helper which will automatically close the overlying loop
class LoopBodyGen {
public:
    LoopBodyGen(LoopGen & loopGen);
    ~LoopBodyGen();

private:
    LoopGen & loopGen;
};

//-----------------------------------------------------------------------------

namespace TypeWrappers {

class Bool {
public:
    typedef bool native_type;

    llvm::Value * llvmValue;

    explicit Bool(bool value)
    {
        auto & codeGen = getThreadLocalCodeGen();
        llvmValue = codeGen->getInt1(value);
    }

    explicit Bool(llvm::Value * value) :
            llvmValue(value)
    {
        assert(value->getType()->isIntegerTy(1));
    }

    Bool(const Bool & other) :
            llvmValue(other.llvmValue)
    { }

    Bool operator ==(const Bool & other)
    {
        auto & codeGen = getThreadLocalCodeGen();
        return Bool( codeGen->CreateICmpEQ(llvmValue, other.llvmValue) );
    }

    Bool operator !=(const Bool & other)
    {
        auto & codeGen = getThreadLocalCodeGen();
        return Bool( codeGen->CreateICmpNE(llvmValue, other.llvmValue) );
    }

    Bool operator !()
    {
        auto & codeGen = getThreadLocalCodeGen();
        return Bool( codeGen->CreateNot(llvmValue) );
    }

    Bool operator &&(const Bool & other)
    {
        auto & codeGen = getThreadLocalCodeGen();
        return Bool( codeGen->CreateAnd(llvmValue, other.llvmValue) );
    }

    Bool operator ||(const Bool & other)
    {
        auto & codeGen = getThreadLocalCodeGen();
        return Bool( codeGen->CreateOr(llvmValue, other.llvmValue) );
    }

    operator llvm::Value *()
    {
        return llvmValue;
    }

    static llvm::Type * getType()
    {
        auto & codeGen = getThreadLocalCodeGen();
        return codeGen->getInt1Ty();
    }
};

//-----------------------------------------------------------------------------

/// IntegerBase is a base class for signed and unsigned integers
template<typename T, typename U>
class IntegerBase {
public:
    typedef T native_type;
    typedef U wrapper_type_t;

    llvm::Value * llvmValue;

    IntegerBase<T, U>()
    {
        auto & codeGen = getThreadLocalCodeGen();
        llvmValue = codeGen->getIntN( sizeof(T)*8, 0 );
    }

    IntegerBase<T, U>(T value)
    {
        auto & codeGen = getThreadLocalCodeGen();
        llvmValue = codeGen->getIntN( sizeof(T)*8, value );
    }

    wrapper_type_t operator +(const wrapper_type_t & other)
    {
        auto & codeGen = getThreadLocalCodeGen();
        return wrapper_type_t( codeGen->CreateAdd(llvmValue, other.llvmValue) );
    }

    wrapper_type_t operator +(T other)
    {
        auto & codeGen = getThreadLocalCodeGen();
        return wrapper_type_t( codeGen->CreateAdd(llvmValue, wrapper_type_t(other)) );
    }

    wrapper_type_t operator -(const wrapper_type_t & other)
    {
        auto & codeGen = getThreadLocalCodeGen();
        return wrapper_type_t( codeGen->CreateSub(llvmValue, other.llvmValue) );
    }

    wrapper_type_t operator -(T other)
    {
        auto & codeGen = getThreadLocalCodeGen();
        return wrapper_type_t( codeGen->CreateSub(llvmValue, wrapper_type_t(other)) );
    }

    wrapper_type_t operator *(const wrapper_type_t & other)
    {
        auto & codeGen = getThreadLocalCodeGen();
        return wrapper_type_t( codeGen->CreateMul(llvmValue, other.llvmValue) );
    }

    wrapper_type_t operator ~()
    {
        auto & codeGen = getThreadLocalCodeGen();
        return wrapper_type_t( codeGen->CreateNot(llvmValue) );
    }

    wrapper_type_t operator &(const wrapper_type_t & other)
    {
        auto & codeGen = getThreadLocalCodeGen();
        return wrapper_type_t( codeGen->CreateAnd(llvmValue, other.llvmValue) );
    }

    wrapper_type_t operator |(const wrapper_type_t & other)
    {
        auto & codeGen = getThreadLocalCodeGen();
        return wrapper_type_t( codeGen->CreateOr(llvmValue, other.llvmValue) );
    }

    wrapper_type_t operator ^(const wrapper_type_t & other)
    {
        auto & codeGen = getThreadLocalCodeGen();
        return wrapper_type_t( codeGen->CreateXor(llvmValue, other.llvmValue) );
    }

    wrapper_type_t operator <<(const wrapper_type_t & other)
    {
        auto & codeGen = getThreadLocalCodeGen();
        return wrapper_type_t( codeGen->CreateShl(llvmValue, other.llvmValue) );
    }

    Bool operator ==(const wrapper_type_t & other)
    {
        auto & codeGen = getThreadLocalCodeGen();
        return Bool( codeGen->CreateICmpEQ(llvmValue, other.llvmValue) );
    }

    Bool operator !=(const wrapper_type_t & other)
    {
        auto & codeGen = getThreadLocalCodeGen();
        return Bool( codeGen->CreateICmpNE(llvmValue, other.llvmValue) );
    }

    operator llvm::Value *()
    {
        return llvmValue;
    }

    llvm::Value * getValue()
    {
        return llvmValue;
    }

    static llvm::Type * getType()
    {
        auto & codeGen = getThreadLocalCodeGen();
        return codeGen->getIntNTy(sizeof(T) * 8);
    }

protected:
    explicit IntegerBase<T, U>(llvm::Value * value) :
            llvmValue(value)
    { }
};

//-----------------------------------------------------------------------------

template<typename T>
class UInteger : public IntegerBase<T, UInteger<T>> {
public:
    typedef UInteger<T> wrapper_type_t;

    UInteger<T>() :
            IntegerBase<T, UInteger<T>>()
    { }

    UInteger<T>(T value) :
            IntegerBase<T, UInteger<T>>(value)
    { }

    explicit UInteger<T>(llvm::Value * value) :
            IntegerBase<T, UInteger<T>>(nullptr)
    {
        auto * otherTy = value->getType();
        assert(otherTy->isIntegerTy());

        auto & codeGen = getThreadLocalCodeGen();

        // expand value if necessary
        unsigned otherSize = otherTy->getPrimitiveSizeInBits();
        unsigned mySize = sizeof(T)*8;
        auto * myTy = codeGen->getIntNTy(mySize);
        if (otherSize < mySize) {
            this->llvmValue = codeGen->CreateZExt(value, myTy);
        } else if (otherSize > mySize) {
            this->llvmValue = codeGen->CreateTrunc(value, myTy);
        } else if (otherSize == mySize) {
            this->llvmValue = value;
        }
    }

    wrapper_type_t operator /(const wrapper_type_t & other)
    {
        auto & codeGen = getThreadLocalCodeGen();
        return wrapper_type_t( codeGen->CreateUDiv(this->llvmValue, other.llvmValue) );
    }

    wrapper_type_t operator %(const wrapper_type_t & other)
    {
        auto & codeGen = getThreadLocalCodeGen();
        return wrapper_type_t( codeGen->CreateURem(this->llvmValue, other.llvmValue) );
    }

    wrapper_type_t operator >>(const wrapper_type_t & other)
    {
        auto & codeGen = getThreadLocalCodeGen();
        return wrapper_type_t( codeGen->CreateLShr(this->llvmValue, other.llvmValue) );
    }

    Bool operator >(const wrapper_type_t & other)
    {
        auto & codeGen = getThreadLocalCodeGen();
        return Bool( codeGen->CreateICmpUGT(this->llvmValue, other.llvmValue) );
    }

    Bool operator <(const wrapper_type_t & other)
    {
        auto & codeGen = getThreadLocalCodeGen();
        return Bool( codeGen->CreateICmpULT(this->llvmValue, other.llvmValue) );
    }

    Bool operator >=(const wrapper_type_t & other)
    {
        auto & codeGen = getThreadLocalCodeGen();
        return Bool( codeGen->CreateICmpUGE(this->llvmValue, other.llvmValue) );
    }

    Bool operator <=(const wrapper_type_t & other)
    {
        auto & codeGen = getThreadLocalCodeGen();
        return Bool( codeGen->CreateICmpULE(this->llvmValue, other.llvmValue) );
    }
};

//-----------------------------------------------------------------------------

template<typename T>
class SInteger : public IntegerBase<T, SInteger<T>> {
public:
    typedef SInteger<T> wrapper_type_t;

    SInteger<T>() :
            IntegerBase<T, SInteger<T>>()
    { }

    SInteger<T>(T value) :
            IntegerBase<T, SInteger<T>>(value)
    { }

    explicit SInteger<T>(llvm::Value * value) :
            IntegerBase<T, SInteger<T>>(nullptr)
    {
        auto * otherTy = value->getType();
        assert(otherTy->isIntegerTy());

        auto & codeGen = getThreadLocalCodeGen();

        // expand value if necessary
        unsigned otherSize = otherTy->getPrimitiveSizeInBits();
        unsigned mySize = sizeof(T)*8;
        auto * myTy = codeGen->getIntNTy(mySize);
        if (otherSize < mySize) {
            this->llvmValue = codeGen->CreateSExt(value, myTy);
        } else if (otherSize > mySize) {
            this->llvmValue = codeGen->CreateTrunc(value, myTy);
        } else if (otherSize == mySize) {
            this->llvmValue = value;
        }
    }

    wrapper_type_t operator/(const wrapper_type_t & other)
    {
        auto & codeGen = getThreadLocalCodeGen();
        return wrapper_type_t( codeGen->CreateSDiv(this->llvmValue, other.llvmValue) );
    }

    wrapper_type_t operator%(const wrapper_type_t & other)
    {
        auto & codeGen = getThreadLocalCodeGen();
        return wrapper_type_t( codeGen->CreateSRem(this->llvmValue, other.llvmValue) );
    }

    wrapper_type_t operator >>(const wrapper_type_t & other)
    {
        auto & codeGen = getThreadLocalCodeGen();
        return wrapper_type_t( codeGen->CreateAShr(this->llvmValue, other.llvmValue) );
    }

    Bool operator >(const wrapper_type_t & other)
    {
        auto & codeGen = getThreadLocalCodeGen();
        return Bool( codeGen->CreateICmpSGT(this->llvmValue, other.llvmValue) );
    }

    Bool operator <(const wrapper_type_t & other)
    {
        auto & codeGen = getThreadLocalCodeGen();
        return Bool( codeGen->CreateICmpSLT(this->llvmValue, other.llvmValue) );
    }

    Bool operator >=(const wrapper_type_t & other)
    {
        auto & codeGen = getThreadLocalCodeGen();
        return Bool( codeGen->CreateICmpSGE(this->llvmValue, other.llvmValue) );
    }

    Bool operator <=(const wrapper_type_t & other)
    {
        auto & codeGen = getThreadLocalCodeGen();
        return Bool( codeGen->CreateICmpSLE(this->llvmValue, other.llvmValue) );
    }
};

//-----------------------------------------------------------------------------

using UInt8 = UInteger<uint8_t>;
using UInt16 = UInteger<uint16_t>;
using UInt32 = UInteger<uint32_t>;
using UInt64 = UInteger<uint64_t>;

using Int8 = SInteger<int8_t>;
using Int16 = SInteger<int16_t>;
using Int32 = SInteger<int32_t>;
using Int64 = SInteger<int64_t>;

using Char = UInt8;

//-----------------------------------------------------------------------------

template<unsigned size> struct SizeSwitch { };
template<> struct SizeSwitch<4> { typedef uint32_t type; };
template<> struct SizeSwitch<8> { typedef uint64_t type; };
using ptr_int_rep_t = SizeSwitch<sizeof(void *)>::type ;
constexpr unsigned ptrBitSize = sizeof(ptr_int_rep_t)*8;

/// PointerBase is a wrapper for pointers to integral types and provides C-like pointer arithmetic
template<unsigned n>
class PointerBase {
public:
    typedef PointerBase<n> wrapper_type_t;

    llvm::Value * llvmValue;

    template<typename T>
    static PointerBase<n> fromRawPointer(T * address)
    {
        auto & codeGen = getThreadLocalCodeGen();
        llvm::Value * intVal = codeGen->getIntN( ptrBitSize, reinterpret_cast<ptr_int_rep_t>(address) );
        llvm::Value * ptrVal = codeGen->CreateIntToPtr(
                intVal, llvm::PointerType::getIntNPtrTy(codeGen.getLLVMContext(), ptrBitSize) );
        return PointerBase<n>(ptrVal);
    }

    PointerBase<n>() :
            llvmValue(nullptr)
    { }

    explicit PointerBase<n>(llvm::Value * value)
    {
        auto * destTy= getType();
        if (value->getType() == destTy) {
            llvmValue = value;
        } else {
            auto & codeGen = getThreadLocalCodeGen();
            llvmValue = codeGen->CreatePointerCast(value, destTy);
        }
    }

    Bool operator ==(const wrapper_type_t & other)
    {
        auto & codeGen = getThreadLocalCodeGen();
        llvm::Value * diff = codeGen->CreatePtrDiff(llvmValue, other.llvmValue);
        return Bool( codeGen->CreateICmpEQ(diff, codeGen->getInt64(0)) );
    }

    Bool operator !=(const wrapper_type_t & other)
    {
        auto & codeGen = getThreadLocalCodeGen();
        llvm::Value * diff = codeGen->CreatePtrDiff(llvmValue, other.llvmValue);
        return Bool( codeGen->CreateICmpNE(diff, codeGen->getInt64(0)) );
    }

    wrapper_type_t operator +(ptr_int_rep_t offset)
    {
        auto & codeGen = getThreadLocalCodeGen();
        auto * intTy = codeGen->getIntNTy(8*sizeof(ptr_int_rep_t));
        auto * ptrTy = getType();

        offset *= n/8; // C-like pointer arithmetic

        // cast to int and add
        llvm::Value * intVal = codeGen->CreatePtrToInt(llvmValue, intTy);
        intVal = codeGen->CreateAdd(intVal, codeGen->getIntN(8*sizeof(ptr_int_rep_t), offset));
        return wrapper_type_t( codeGen->CreateIntToPtr(intVal, ptrTy) );
    }

    wrapper_type_t operator +(llvm::Value * offset)
    {
        assert(offset->getType()->isIntegerTy());

        auto & codeGen = getThreadLocalCodeGen();
        auto * intTy = codeGen->getIntNTy(8*sizeof(ptr_int_rep_t));
        auto * ptrTy = getType();

        llvm::Value * intVal = codeGen->CreatePtrToInt(llvmValue, intTy);

        uint32_t factor = n/8; // C-like pointer arithmetic
        if (factor > 1) {
            offset = codeGen->CreateMul(offset, codeGen->getInt32(factor));
        }

        intVal = codeGen->CreateAdd(intVal, offset);
        return wrapper_type_t( codeGen->CreateIntToPtr(intVal, ptrTy) );
    }

    wrapper_type_t operator -(ptr_int_rep_t offset)
    {
        auto & codeGen = getThreadLocalCodeGen();
        auto * intTy = codeGen->getIntNTy(8*sizeof(ptr_int_rep_t));
        auto * ptrTy = getType();

        offset *= n/8; // C-like pointer arithmetic

        // cast to int and sub
        llvm::Value * intVal = codeGen->CreatePtrToInt(llvmValue, intTy);
        intVal = codeGen->CreateSub(intVal, codeGen->getIntN(8*sizeof(ptr_int_rep_t), offset));
        return wrapper_type_t( codeGen->CreateIntToPtr(intVal, ptrTy) );
    }

    wrapper_type_t operator -(llvm::Value * offset)
    {
        assert(offset->getType()->isIntegerTy());

        auto & codeGen = getThreadLocalCodeGen();
        auto * intTy = codeGen->getIntNTy(8*sizeof(ptr_int_rep_t));
        auto * ptrTy = getType();

        llvm::Value * intVal = codeGen->CreatePtrToInt(llvmValue, intTy);

        uint32_t factor = n/8; // C-like pointer arithmetic
        if (factor > 1) {
            offset = codeGen->CreateMul(offset, codeGen->getInt32(factor));
        }

        intVal = codeGen->CreateSub(intVal, offset);
        return wrapper_type_t( codeGen->CreateIntToPtr(intVal, ptrTy) );
    }

    operator llvm::Value *()
    {
        return llvmValue;
    }

    llvm::Value * getValue()
    {
        return llvmValue;
    }

    Bool isNullPtr()
    {
        auto & codeGen = getThreadLocalCodeGen();
        auto & context = codeGen.getLLVMContext();
        llvm::PointerType * type = llvm::PointerType::getIntNPtrTy(context, n);
        llvm::Value * constantNull = llvm::ConstantPointerNull::get(type);
        return Bool( codeGen->CreateICmpEQ(llvmValue, constantNull) );
    }

    static llvm::Type * getType()
    {
        auto & codeGen = getThreadLocalCodeGen();
        return llvm::PointerType::getIntNPtrTy(codeGen.getLLVMContext(), n);
    }
};

using Ptr8 = PointerBase<8>;
using Ptr16 = PointerBase<16>;
using Ptr32 = PointerBase<32>;
using Ptr64 = PointerBase<64>;

using VoidPtr = Ptr8;

} // end namespace TypeWrappers

using ptr_int_rep_t = TypeWrappers::ptr_int_rep_t;

//-----------------------------------------------------------------------------
// CodeGen types:

using cg_bool_t = TypeWrappers::Bool;

using cg_int_t = TypeWrappers::SInteger<int>;
using cg_unsigned_t = TypeWrappers::UInteger<unsigned>;
using cg_char_t = TypeWrappers::Char;

using cg_u8_t = TypeWrappers::UInt8;
using cg_u16_t = TypeWrappers::UInt16;
using cg_u32_t = TypeWrappers::UInt32;
using cg_u64_t = TypeWrappers::UInt64;

using cg_i8_t = TypeWrappers::Int8;
using cg_i16_t = TypeWrappers::Int16;
using cg_i32_t = TypeWrappers::Int32;
using cg_i64_t = TypeWrappers::Int64;

using cg_ptr8_t = TypeWrappers::Ptr8;
using cg_ptr16_t = TypeWrappers::Ptr16;
using cg_ptr32_t = TypeWrappers::Ptr32;
using cg_ptr64_t = TypeWrappers::Ptr64;

using cg_voidptr_t = TypeWrappers::VoidPtr;

using cg_hash_t = TypeWrappers::UInt64;
using cg_size_t = TypeWrappers::UInt64;

//-----------------------------------------------------------------------------

namespace Functions {

llvm::Function * getPrintfPrototype(llvm::Module & module);

template<typename... Params>
void genPrintfCall(const std::string & fmt, Params && ... parameters);

cg_int_t genPutcharCall(cg_int_t c);

cg_int_t genMemcmpCall(cg_voidptr_t s1, cg_voidptr_t s2, cg_size_t n);

cg_voidptr_t genMallocCall(cg_size_t size);

void genFreeCall(cg_voidptr_t ptr);

} // end namespace Functions

//-----------------------------------------------------------------------------

template<typename T>
llvm::Value * createPointerValue(T * ptr, llvm::Type * elementTy);

//-----------------------------------------------------------------------------

#include "CodeGen.tcc"

#endif // LLVM_PROTOTYPE_CODEGEN_H
