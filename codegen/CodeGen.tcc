
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
