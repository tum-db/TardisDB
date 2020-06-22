//
// Created by josef on 30.12.16.
//

#include <llvm/IR/TypeBuilder.h>
#include <iostream>

#include "codegen/CodeGen.hpp"
#include "queryExecutor/queryExecutor.hpp"

#include "gtest/gtest.h"

llvm::Function *codegen_addition_test() {
    auto &codeGen = getThreadLocalCodeGen();
    auto &context = codeGen.getLLVMContext();
    auto &moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType *funcTy = llvm::TypeBuilder<int(), false>::get(context);
    FunctionGen funcGen(moduleGen, "codegenAdditionTest", funcTy);

    using namespace TypeWrappers;

    // Types test:
    Int32 i = Int32(1) + Int32(1);

    funcGen.setReturnValue(i);

    return funcGen.getFunction();
}

llvm::Function *codegen_multiplication_test() {
    auto &codeGen = getThreadLocalCodeGen();
    auto &context = codeGen.getLLVMContext();
    auto &moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType *funcTy = llvm::TypeBuilder<int(), false>::get(context);
    FunctionGen funcGen(moduleGen, "codegenMultiplicationTest", funcTy);

    using namespace TypeWrappers;

    Int32 i = Int32(6) * Int32(7);

    funcGen.setReturnValue(i);

    return funcGen.getFunction();
}

llvm::Function *codegen_equal_test() {
    auto &codeGen = getThreadLocalCodeGen();
    auto &context = codeGen.getLLVMContext();
    auto &moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType *funcTy = llvm::TypeBuilder<int(), false>::get(context);
    FunctionGen funcGen(moduleGen, "codegenEqualTest", funcTy);

    using namespace TypeWrappers;

    Bool r = (Bool(true) == Bool(true));

    Functions::genPrintfCall("Equal test succeeded: %d",r);

    funcGen.setReturnValue(r);

    return funcGen.getFunction();
}

llvm::Function *codegen_equal2_test() {
    auto &codeGen = getThreadLocalCodeGen();
    auto &context = codeGen.getLLVMContext();
    auto &moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType *funcTy = llvm::TypeBuilder<int(), false>::get(context);
    FunctionGen funcGen(moduleGen, "codegenEqual2Test", funcTy);

    using namespace TypeWrappers;

    Bool r = (Bool(true) == Bool(false));

    funcGen.setReturnValue(r);

    return funcGen.getFunction();
}

TEST(CodegenTest, AdditionTest) {
    ModuleGen moduleGen("CodegenAdditionTestModule");
    std::vector<llvm::GenericValue> args;
    llvm::Function *additionTest = codegen_addition_test();
    llvm::GenericValue resultAdditionTest = QueryExecutor::executeFunction(additionTest, args);
    ASSERT_EQ(resultAdditionTest.IntVal.getZExtValue(), 2);
}

TEST(CodegenTest, MultiplicationTest) {
    ModuleGen moduleGen("CodegenMultiplicationTestModule");
    std::vector<llvm::GenericValue> args;
    llvm::Function *multiplicationTest = codegen_multiplication_test();
    llvm::GenericValue resultMultiplicationTest = QueryExecutor::executeFunction(multiplicationTest, args);
    ASSERT_EQ(resultMultiplicationTest.IntVal.getZExtValue(), 42);
}

TEST(CodegenTest, EqualTest) {
    ModuleGen moduleGen("CodegenEqualTestModule");
    std::vector<llvm::GenericValue> args;
    llvm::Function *equalTest = codegen_equal_test();
    llvm::GenericValue resultEqualTest = QueryExecutor::executeFunction(equalTest, args);
    ASSERT_EQ(resultEqualTest.IntVal.getZExtValue(), 1);
}

TEST(CodegenTest, Equal2Test) {
    ModuleGen moduleGen("CodegenEqual2TestModule");
    std::vector<llvm::GenericValue> args;
    llvm::Function *equal2Test = codegen_equal2_test();
    llvm::GenericValue resultEqual2Test = QueryExecutor::executeFunction(equal2Test, args);
    ASSERT_EQ(resultEqual2Test.IntVal.getZExtValue(), 0);
}

void executeCodegenAdditionTest() {
    ModuleGen moduleGen("CodegenAdditionTestModule");
    std::vector<llvm::GenericValue> args;
    llvm::Function *additionTest = codegen_addition_test();
    llvm::GenericValue resultAdditionTest = QueryExecutor::executeFunction(additionTest, args);
    std::cout << "1+1: " << resultAdditionTest.IntVal.getZExtValue() << "\n";
}

void executeCodegenMultiplicationTest() {
    ModuleGen moduleGen("CodegenMultiplicationTestModule");
    std::vector<llvm::GenericValue> args;
    llvm::Function *multiplicationTest = codegen_multiplication_test();
    llvm::GenericValue resultMultiplicationTest = QueryExecutor::executeFunction(multiplicationTest, args);
    std::cout << "6*7: " << resultMultiplicationTest.IntVal.getZExtValue() << "\n";
}

void executeCodegenEqualTest() {
    ModuleGen moduleGen("CodegenEqualTestModule");
    std::vector<llvm::GenericValue> args;
    llvm::Function *equalTest = codegen_equal_test();
    llvm::GenericValue resultEqualTest = QueryExecutor::executeFunction(equalTest, args);
    std::cout << "Bool(true) == Bool(true): " << resultEqualTest.IntVal.getZExtValue() << "\n";
}

void executeCodegenEqual2Test() {
    ModuleGen moduleGen("CodegenEqual2TestModule");
    std::vector<llvm::GenericValue> args;
    llvm::Function *equal2Test = codegen_equal2_test();
    llvm::GenericValue resultEqual2Test = QueryExecutor::executeFunction(equal2Test, args);
    std::cout << "Bool(true) == Bool(false): " << resultEqual2Test.IntVal.getZExtValue() << "\n";
}

void codegen_test() {
    executeCodegenAdditionTest();
    executeCodegenMultiplicationTest();
    executeCodegenEqualTest();
    executeCodegenEqual2Test();
}