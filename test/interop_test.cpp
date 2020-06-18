//
// Created by josef on 30.12.16.
//

#include <llvm/IR/TypeBuilder.h>
#include <iostream>

#include "codegen/CodeGen.hpp"
#include "query_compiler/compiler.hpp"
//#include "gtest/gtest.h"

extern "C" const char* interop2()
{
    return "=== interop ===";
}

static const char* mangled_interop2()
{
    return "=== mangled_interop ===";
}

int interop_compare(const char* result)
{
    return std::string(result).compare("=== interop ===");
}

static int mangled_interop_compare(const char* result)
{
    return std::string(result).compare("=== mangled_interop ===");
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
llvm::Function * genInterop1Test()
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType * voidFuncTy = llvm::TypeBuilder<void (), false>::get(context);
    FunctionGen funcGen(moduleGen, "interop1Test", voidFuncTy);

    llvm::FunctionType * funcGenerateStringTy = llvm::TypeBuilder<char * (), false>::get(codeGen.getLLVMContext());
    llvm::CallInst * generatedStringWrapped = codeGen.CreateCall(&interop2, funcGenerateStringTy);
    cg_voidptr_t generatedString = cg_voidptr_t( llvm::cast<llvm::Value>(generatedStringWrapped) );

    llvm::FunctionType * funcStringCompareTy = llvm::TypeBuilder<int (char*), false>::get(codeGen.getLLVMContext());
    llvm::CallInst * compareResultWrapped = codeGen.CreateCall(&interop_compare, funcStringCompareTy, {generatedString});

    cg_i32_t result = cg_i32_t( llvm::cast<llvm::Value>(compareResultWrapped) );

    funcGen.setReturnValue(codeGen->CreateICmpEQ(result,cg_i32_t(0)));
/*
    llvm::FunctionType * memberFuncTy = llvm::TypeBuilder<void(void *, int), false>::get(context);
    cg_voidptr_t inst = cg_voidptr_t::fromRawPointer(&testClassInstance);
    codeGen.CreateCall(&TestClass::testFunc, memberFuncTy, {inst, cg_int_t(3)});
*/
    return funcGen.getFunction();
}

llvm::Function * genInterop2Test()
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType * voidFuncTy = llvm::TypeBuilder<void (), false>::get(context);
    FunctionGen funcGen(moduleGen, "interop2Test", voidFuncTy);

    llvm::FunctionType * funcGenerateStringTy = llvm::TypeBuilder<char * (), false>::get(codeGen.getLLVMContext());
    llvm::CallInst * generatedStringWrapped = codeGen.CreateCall(&mangled_interop2, funcGenerateStringTy);
    cg_voidptr_t generatedString = cg_voidptr_t( llvm::cast<llvm::Value>(generatedStringWrapped) );

    llvm::FunctionType * funcStringCompareTy = llvm::TypeBuilder<int (char*), false>::get(codeGen.getLLVMContext());
    llvm::CallInst * compareResultWrapped = codeGen.CreateCall(&mangled_interop_compare, funcStringCompareTy, {generatedString});

    cg_i32_t result = cg_i32_t( llvm::cast<llvm::Value>(compareResultWrapped) );

    funcGen.setReturnValue(codeGen->CreateICmpEQ(result,cg_i32_t(0)));

    return funcGen.getFunction();
}

/*TEST(InteropTest, InteropTest1) {
    ModuleGen moduleGen("Interop1TestModule");
    std::vector<llvm::GenericValue> args;
    llvm::Function * interopTest = genInterop1Test();
    llvm::GenericValue resultinteropTest = QueryCompiler::compileAndExecuteReturn(interopTest,args);
    ASSERT_EQ(resultinteropTest.IntVal.getZExtValue(),1);
}

TEST(InteropTest, InteropTest2) {
    ModuleGen moduleGen("Interop2TestModule");
    std::vector<llvm::GenericValue> args;
    llvm::Function * interopTest = genInterop2Test();
    llvm::GenericValue resultinteropTest = QueryCompiler::compileAndExecuteReturn(interopTest,args);
    ASSERT_EQ(resultinteropTest.IntVal.getZExtValue(),1);
}*/

void executeInterop1Test() {
    ModuleGen moduleGen("Interop1TestModule");
    std::vector<llvm::GenericValue> args;
    llvm::Function * interopTest = genInterop1Test();
    llvm::GenericValue resultinteropTest = QueryCompiler::compileAndExecuteReturn(interopTest,args);
    std::cout << "test1: passed: " << resultinteropTest.IntVal.getZExtValue() << "\n";
}

void executeInterop2Test() {
    ModuleGen moduleGen("Interop2TestModule");
    std::vector<llvm::GenericValue> args;
    llvm::Function * interopTest = genInterop2Test();
    llvm::GenericValue resultinteropTest = QueryCompiler::compileAndExecuteReturn(interopTest,args);
    std::cout << "test2: passed: " << resultinteropTest.IntVal.getZExtValue() << "\n";
}

void interop_test2() {
    executeInterop1Test();
    executeInterop2Test();
}