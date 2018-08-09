//
// Created by josef on 30.12.16.
//

#include <llvm/IR/TypeBuilder.h>

#include "CodeGen.hpp"

extern "C" void interop()
{
    printf("\n=== interop ===\n\n");
    fflush(stdout);
}

void mangled_interop()
{
    printf("\n=== mangled_interop ===\n\n");
    fflush(stdout);
}
/*
class TestClass {
public:
    int a = 2;
    void testFunc(int b);
};

void TestClass::testFunc(int b)
{
    printf("TestClass::testFunc: a+b = %d\n", a+b);
    fflush(stdout);
}

static TestClass testClassInstance;
*/
llvm::Function * interop_test()
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType * voidFuncTy = llvm::TypeBuilder<void(), false>::get(context);
    FunctionGen funcGen(moduleGen, "interopTest", voidFuncTy);

    codeGen.CreateCall(&interop, voidFuncTy);
    codeGen.CreateCall(&mangled_interop, voidFuncTy);
/*
    llvm::FunctionType * memberFuncTy = llvm::TypeBuilder<void(void *, int), false>::get(context);
    cg_voidptr_t inst = cg_voidptr_t::fromRawPointer(&testClassInstance);
    codeGen.CreateCall(&TestClass::testFunc, memberFuncTy, {inst, cg_int_t(3)});
*/
    return funcGen.getFunction();
}
