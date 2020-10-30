
template<typename Func>
llvm::CallInst * CodeGen::CreateCall(
        Func f,
        llvm::FunctionType * FTy,
        const llvm::Twine & Name,
        llvm::MDNode * FPMathTag)
{
    return CreateCall(f, FTy, {}, Name,FPMathTag);
}

template<typename Func>
llvm::CallInst * CodeGen::CreateCall(
        Func f,
        llvm::FunctionType * FTy,
        llvm::ArrayRef<llvm::Value *> Args,
        const llvm::Twine & Name,
        llvm::MDNode * FPMathTag)
{
    llvm::Type * ptrTy = llvm::PointerType::getUnqual(FTy);
    llvm::Constant * address = builder.getIntN( sizeof(void *)*8, reinterpret_cast<ptr_int_rep_t >(f) );
    llvm::Value * calee = builder.CreateIntToPtr(address, ptrTy);

    return builder.CreateCall(FTy, calee, Args, Name, FPMathTag);
}

//-----------------------------------------------------------------------------
// some TypeTranslator<> specialisations

template<>
struct TypeTranslator<int8_t> {
    static llvm::Type* get(llvm::LLVMContext& ctx) { llvm::Type::getInt8Ty(ctx); }
};

template<>
struct TypeTranslator<int16_t> {
    static llvm::Type* get(llvm::LLVMContext& ctx) { llvm::Type::getInt16Ty(ctx); }
};

template<>
struct TypeTranslator<int32_t> {
    static llvm::Type* get(llvm::LLVMContext& ctx) { llvm::Type::getInt32Ty(ctx); }
};

template<>
struct TypeTranslator<int64_t> {
    static llvm::Type* get(llvm::LLVMContext& ctx) { llvm::Type::getInt64Ty(ctx); }
};

template<>
struct TypeTranslator<uint8_t> {
    static llvm::Type* get(llvm::LLVMContext& ctx) { llvm::Type::getInt8Ty(ctx); }
};

template<>
struct TypeTranslator<uint16_t> {
    static llvm::Type* get(llvm::LLVMContext& ctx) { llvm::Type::getInt16Ty(ctx); }
};

template<>
struct TypeTranslator<uint32_t> {
    static llvm::Type* get(llvm::LLVMContext& ctx) { llvm::Type::getInt32Ty(ctx); }
};

template<>
struct TypeTranslator<uint64_t> {
    static llvm::Type* get(llvm::LLVMContext& ctx) { llvm::Type::getInt64Ty(ctx); }
};

template<>
struct TypeTranslator<float> {
    static llvm::Type* get(llvm::LLVMContext& ctx) { llvm::Type::getFloatTy(ctx); }
};

template<>
struct TypeTranslator<double> {
    static llvm::Type* get(llvm::LLVMContext& ctx) { llvm::Type::getDoubleTy(ctx); }
};

//-----------------------------------------------------------------------------

template<class... Ts>
constexpr auto translateTypes(llvm::LLVMContext& ctx)
    -> std::array<llvm::Type*, sizeof...(Ts)>
{
    return { TypeTranslator<Ts>::get(ctx)... };
}

template<class T>
llvm::Type* translateType(llvm::LLVMContext& ctx, T&& t) {
    return TypeTranslator<T>::get(ctx);
}

template<llvm::Value*>
llvm::Type* translateType(llvm::LLVMContext& ctx, llvm::Value* value) {
    return value->getType();
}

template<class... Ts>
constexpr auto translateTypes(llvm::LLVMContext& ctx, Ts&&... ts)
    -> std::array<llvm::Type*, sizeof...(Ts)>
{
    return { translateType(ctx, std::forward<Ts>(ts))... };
}

template<class R, class... Args>
struct TypeTranslator<R(Args...)> {
    static llvm::FunctionType* get(llvm::LLVMContext& ctx) {
        // construct llvm function type
        auto returnType = TypeTranslator<R>::get(ctx);
        auto argumentTypes = translateTypes<Args...>(ctx);
        auto functionType = llvm::FunctionType::get(returnType, argumentTypes, false);
        return functionType;
    }
};

template<class R, class... Args>
struct TypeTranslator<R(*)(Args...)> {
    static llvm::FunctionType* get(llvm::LLVMContext& ctx) {
        // construct llvm function type
        auto returnType = TypeTranslator<R>::get(ctx);
        auto argumentTypes = translateTypes<Args...>(ctx);
        auto functionType = llvm::FunctionType::get(returnType, argumentTypes, false);
        return functionType;
    }
};

template<class R, class U, class... Args>
struct TypeTranslator<R(U::*)(Args...)> {
    static llvm::FunctionType* get(llvm::LLVMContext& ctx) {
        // construct llvm function type
        auto returnType = TypeTranslator<R>::get(ctx);
        auto argumentTypes = translateTypes<Args...>(ctx);
        auto functionType = llvm::FunctionType::get(returnType, argumentTypes, false);
        return functionType;
    }
};

//-----------------------------------------------------------------------------
// some ConstantTranslator<> specialisations

template<>
struct ConstantTranslator<int8_t> {
    static llvm::Value* get(llvm::LLVMContext& ctx, int8_t value) { return llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx), value, true); }
};

template<>
struct ConstantTranslator<int16_t> {
    static llvm::Value* get(llvm::LLVMContext& ctx, int16_t value) { return llvm::ConstantInt::get(llvm::Type::getInt16Ty(ctx), value, true); }
};

template<>
struct ConstantTranslator<int32_t> {
    static llvm::Value* get(llvm::LLVMContext& ctx, int32_t value) { return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), value, true); }
};

template<>
struct ConstantTranslator<int64_t> {
    static llvm::Value* get(llvm::LLVMContext& ctx, int64_t value) { return llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx), value, true); }
};

template<>
struct ConstantTranslator<uint8_t> {
    static llvm::Value* get(llvm::LLVMContext& ctx, uint8_t value) { return llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx), value, true); }
};

template<>
struct ConstantTranslator<uint16_t> {
    static llvm::Value* get(llvm::LLVMContext& ctx, uint16_t value) { return llvm::ConstantInt::get(llvm::Type::getInt16Ty(ctx), value, true); }
};

template<>
struct ConstantTranslator<uint32_t> {
    static llvm::Value* get(llvm::LLVMContext& ctx, uint32_t value) { return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), value, true); }
};

template<>
struct ConstantTranslator<uint64_t> {
    static llvm::Value* get(llvm::LLVMContext& ctx, uint64_t value) { return llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx), value, true); }
};

template<>
struct ConstantTranslator<float> {
    static llvm::Value* get(llvm::LLVMContext& ctx, float value) { return llvm::ConstantFP::get(llvm::Type::getFloatTy(ctx), value); }
};

template<>
struct ConstantTranslator<double> {
    static llvm::Value* get(llvm::LLVMContext& ctx, double value) { return llvm::ConstantFP::get(llvm::Type::getDoubleTy(ctx), value); }
};

template<>
struct ConstantTranslator<void*> {
    static llvm::Value* get(llvm::LLVMContext& ctx, void* value) {
        // check if this is a 64 bit arch
        static_assert(sizeof(void*) == 8);
        llvm::Constant* address = llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx), (int64_t)value);
        return llvm::ConstantExpr::getIntToPtr(address , llvm::PointerType::getUnqual(llvm::Type::getInt64Ty(ctx))); 
    }
};

template<>
struct ConstantTranslator<llvm::Value*> {
    static llvm::Value* get(llvm::LLVMContext& ctx, llvm::Value* value) { return value; }
};

//-----------------------------------------------------------------------------

template<class... Ts>
constexpr auto translateValues(llvm::LLVMContext& ctx, Ts&&... ts)
    -> std::array<llvm::Value*, sizeof...(Ts)>
{
    return { ConstantTranslator<Ts>::get(ctx, std::forward<Ts>(ts))... };
}

//-----------------------------------------------------------------------------

template<typename Func, class... Args>
llvm::Value* CodeGen::genCall(Func f, Args... args) {
    auto & ctx = getLLVMContext();

    // one alternative to the following would be the use of: https://stackoverflow.com/a/3104792
#pragma GCC diagnostic ignored "-Wpmf-conversions"
    void* funcPtr = reinterpret_cast<void*>(f);
#pragma GCC diagnostic pop

    auto functionName = getFunctionName(funcPtr);
    //printf("function name: %s\n", functionName.c_str());
    auto functionType = TypeTranslator<Func>::get(ctx);
    //printf("call with numargs: %d\n", functionType->getNumParams());
    auto functionArgs = translateValues<Args...>(ctx, std::forward<Args>(args)...);

    // check if there is already a mapping for the given function
    llvm::Function * functionObj = getCurrentModuleGen().getModule().getFunction(functionName);
    if (!functionObj) {
        functionObj = llvm::cast<llvm::Function>(getCurrentModuleGen().getModule().getOrInsertFunction("test_fun", functionType));
        getCurrentModuleGen().addFunctionMapping(functionObj, funcPtr);
    }

    llvm::CallInst* resultWrapped = builder.CreateCall(functionObj, functionArgs);
    //printf("call generated\n");
    return llvm::cast<llvm::Value>(resultWrapped);
}

//-----------------------------------------------------------------------------

namespace Functions {

template<typename... Params>
void genPrintfCall(const std::string & fmt, Params && ... parameters)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & module = codeGen.getCurrentModuleGen().getModule();

    llvm::Function * func = getPrintfPrototype(module);
    cg_ptr8_t fmtValue( codeGen->CreateGlobalString(fmt) ); // explicit cast to i8*

    // pass the parameters
    codeGen->CreateCall(func, { fmtValue, std::forward<Params>(parameters)... });
}

} // end namespace Functions

//-----------------------------------------------------------------------------

template<typename T>
llvm::Value * createPointerValue(T * ptr, llvm::Type * elementTy)
{
    auto & codeGen = getThreadLocalCodeGen();

    llvm::Type * elementPtrTy = llvm::PointerType::getUnqual(elementTy);
    llvm::Constant * addr = codeGen->getIntN( sizeof(void *)*8, reinterpret_cast<ptr_int_rep_t>(ptr) );
    return codeGen->CreateIntToPtr(addr, elementPtrTy);
}
