//
// Created by josef
//

#include <bits/stdint-intn.h>
#include <llvm/IR/Constant.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/TypeBuilder.h>
#include <llvm/IR/Value.h>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>
#include <dlfcn.h>

#include "codegen/CodeGen.hpp"
#include "queryExecutor/queryExecutor.hpp"
#include "gtest/gtest.h"

int f(double a, float b) {
    //printf("in f with a: %lf b: %f\n", a, b);
    assert(a == 1.);
    assert(b == 2.f);
    return 42;
}

struct S {
    int value;
    void g(int) {};
    int h(float in) {
        //printf("in S::h with in: %f and value: %d\n", in, value);
        assert(in = 3.14f);
        return value;
    };
};

static S s;

static llvm::Function * genInterop1Test() {
    auto & codeGen = getThreadLocalCodeGen();
    auto & ctx = codeGen.getLLVMContext();

//    llvm::FunctionType * voidFuncTy = llvm::TypeBuilder<int (), false>::get(context);
    llvm::FunctionType* testFuncTy = TypeTranslator<int()>::get(ctx);
    FunctionGen funcGen(codeGen.getCurrentModuleGen(), "Interop1Test", testFuncTy);

    llvm::Value* result = codeGen.genCall(f, 1., 2.f);

    funcGen.setReturnValue(result);
    return funcGen.getFunction();
}

static llvm::Function * genInterop2Test() {
    auto & codeGen = getThreadLocalCodeGen();
    auto & ctx = codeGen.getLLVMContext();

    llvm::FunctionType* testFuncTy = TypeTranslator<int()>::get(ctx);
    FunctionGen funcGen(codeGen.getCurrentModuleGen(), "Interop2Test", testFuncTy);

    llvm::Value* fp = llvm::ConstantFP::get(llvm::Type::getFloatTy(ctx), 2.);
    llvm::Value* result = codeGen.genCall(f, 1., fp);

    funcGen.setReturnValue(result);
    return funcGen.getFunction();
}

static llvm::Function * genInterop3Test() {
    auto & codeGen = getThreadLocalCodeGen();
    auto & ctx = codeGen.getLLVMContext();

    llvm::FunctionType* testFuncTy = TypeTranslator<int()>::get(ctx);
    FunctionGen funcGen(codeGen.getCurrentModuleGen(), "Interop3Test", testFuncTy);

    s.value = 42;
    llvm::Value* fp = llvm::ConstantFP::get(llvm::Type::getFloatTy(ctx), 3.14);
    llvm::Value* result = codeGen.genCall(&S::h, (void*)&s, fp);

    funcGen.setReturnValue(result);
    return funcGen.getFunction();
}

TEST(InteropTest, FunctionCallWithConstants) {
    ModuleGen moduleGen("Interop1TestModule");
    std::vector<llvm::GenericValue> args;
    llvm::Function * interopTest = genInterop1Test();
    llvm::GenericValue resultInteropTest = QueryExecutor::executeFunction(interopTest, args);
    ASSERT_EQ(resultInteropTest.IntVal.getZExtValue(), 42);
}

TEST(InteropTest, FunctionCallWithMixedArguments) {
    ModuleGen moduleGen("Interop2TestModule");
    std::vector<llvm::GenericValue> args;
    llvm::Function * interopTest = genInterop2Test();
    llvm::GenericValue resultInteropTest = QueryExecutor::executeFunction(interopTest, args);
    ASSERT_EQ(resultInteropTest.IntVal.getZExtValue(), 42);
}

TEST(InteropTest, MethodCall) {
    ModuleGen moduleGen("Interop3TestModule");
    std::vector<llvm::GenericValue> args;
    llvm::Function * interopTest = genInterop3Test();
    llvm::GenericValue resultInteropTest = QueryExecutor::executeFunction(interopTest, args);
    ASSERT_EQ(resultInteropTest.IntVal.getZExtValue(), 42);
}
