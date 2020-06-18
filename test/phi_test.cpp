
#include <llvm/ADT/STLExtras.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/GenericValue.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/TypeBuilder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Analysis/Passes.h>
#include <llvm/Analysis/LoopPass.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Scalar/GVN.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/IPO/FunctionAttrs.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Target/TargetMachine.h>
#include <iostream>

#include "codegen/CodeGen.hpp"
#include "codegen/PhiNode.hpp"
#include "utils/general.hpp"
#include "gtest/gtest.h"
#include "queryExecutor/queryExecutor.hpp"

using namespace llvm;

static llvm::Function * phi_1_test()
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void (), false>::get(context);
    FunctionGen funcGen(moduleGen, "phi1Test", funcTy);

    {
        PhiNode<llvm::Value> node( codeGen->getInt32(42), "node" );
        IfGen check(funcGen, cg_bool_t(true));
        {
            node = codeGen->getInt32(7);
        }
        check.EndIf();

        cg_i32_t result( node.get() );

        funcGen.setReturnValue(codeGen->CreateICmpEQ(result, cg_i32_t(7)));
    }

    return funcGen.getFunction();
}

static llvm::Function * phi_2_test()
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void (), false>::get(context);
    FunctionGen funcGen(moduleGen, "phi2Test", funcTy);

    {
        PhiNode<llvm::Value> node( codeGen->getInt32(42), "node" );
        IfGen check(funcGen, cg_bool_t(false));
        {
            node = codeGen->getInt32(7);
        }
        check.EndIf();

        cg_i32_t result( node.get() );

        funcGen.setReturnValue(codeGen->CreateICmpEQ(result, cg_i32_t(42)));
    }

    return funcGen.getFunction();
}

static llvm::Function * phi_3_test()
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void (), false>::get(context);
    FunctionGen funcGen(moduleGen, "phi3Test", funcTy);

    {
        PhiNode<llvm::Value> node( codeGen->getInt32(42), "node" );
        IfGen check(funcGen, cg_bool_t(false));
        {
            node = codeGen->getInt32(7);
        }
        check.EndIf();

        cg_i32_t result( node.get() );

        funcGen.setReturnValue(codeGen->CreateICmpEQ(result, cg_i32_t(42)));
    }

    return funcGen.getFunction();
}

static llvm::Function * phi_4_test()
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void (), false>::get(context);
    FunctionGen funcGen(moduleGen, "phi4Test", funcTy);

    {
        PhiNode<llvm::Value> node( codeGen->getInt32(0), "node" );
        IfGen check(funcGen, cg_bool_t(false));
        {
            node = codeGen->getInt32(7);
        }
        check.Else();
        {
            node = codeGen->getInt32(42);
        }
        check.EndIf();

        cg_i32_t result( node.get() );
        funcGen.setReturnValue(codeGen->CreateICmpEQ(result, cg_i32_t(42)));
    }

    return funcGen.getFunction();
}

static llvm::Function * phi_5_test()
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void (), false>::get(context);
    FunctionGen funcGen(moduleGen, "phi5Test", funcTy);

    {
        using namespace Sql;

        value_op_t ival( new Integer(42) );
        PhiNode<Sql::Value> node(*ival, "node");

        IfGen check(funcGen, cg_bool_t(true));
        {
            value_op_t ival2( new Integer(7) );
            node = *ival2;
        }
        check.EndIf();

        value_op_t result( node.get() );

        funcGen.setReturnValue(codeGen->CreateICmpEQ(result->getLLVMValue(), cg_i32_t(7)));
    }

    return funcGen.getFunction();
}

TEST(PhiTest, Phi1Test) {
    ModuleGen moduleGen("Phi1TestModule");
    std::vector<llvm::GenericValue> args;
    llvm::Function * phiTest = phi_1_test();
    llvm::GenericValue resultphiTest = QueryExecutor::executeFunction(phiTest,args);
    ASSERT_EQ(resultphiTest.IntVal.getZExtValue(),1);
}

TEST(PhiTest, Phi2Test) {
    ModuleGen moduleGen("Phi2TestModule");
    std::vector<llvm::GenericValue> args;
    llvm::Function * phiTest = phi_2_test();
    llvm::GenericValue resultphiTest = QueryExecutor::executeFunction(phiTest,args);
    ASSERT_EQ(resultphiTest.IntVal.getZExtValue(),1);
}

TEST(PhiTest, Phi3Test) {
    ModuleGen moduleGen("Phi3TestModule");
    std::vector<llvm::GenericValue> args;
    llvm::Function * phiTest = phi_3_test();
    llvm::GenericValue resultphiTest = QueryExecutor::executeFunction(phiTest,args);
    ASSERT_EQ(resultphiTest.IntVal.getZExtValue(),1);
}

TEST(PhiTest, Phi5Test) {
    ModuleGen moduleGen("Phi5TestModule");
    std::vector<llvm::GenericValue> args;
    llvm::Function * phiTest = phi_5_test();
    llvm::GenericValue resultphiTest = QueryExecutor::executeFunction(phiTest,args);
    ASSERT_EQ(resultphiTest.IntVal.getZExtValue(),1);
}

void executePhi1Test() {
    ModuleGen moduleGen("Phi1TestModule");
    std::vector<llvm::GenericValue> args;
    llvm::Function * phiTest = phi_1_test();
    llvm::GenericValue resultphiTest = QueryExecutor::executeFunction(phiTest,args);
    std::cout << "test1: passed: " << resultphiTest.IntVal.getZExtValue() << "\n";
}

void executePhi2Test() {
    ModuleGen moduleGen("Phi2TestModule");
    std::vector<llvm::GenericValue> args;
    llvm::Function * phiTest = phi_2_test();
    llvm::GenericValue resultphiTest = QueryExecutor::executeFunction(phiTest,args);
    std::cout << "test2: passed: " << resultphiTest.IntVal.getZExtValue() << "\n";
}

void executePhi3Test() {
    ModuleGen moduleGen("Phi3TestModule");
    std::vector<llvm::GenericValue> args;
    llvm::Function * phiTest = phi_3_test();
    llvm::GenericValue resultphiTest = QueryExecutor::executeFunction(phiTest,args);
    std::cout << "test3: passed: " << resultphiTest.IntVal.getZExtValue() << "\n";
}

void executePhi4Test() {
    ModuleGen moduleGen("Phi4TestModule");
    std::vector<llvm::GenericValue> args;
    llvm::Function * phiTest = phi_4_test();
    llvm::GenericValue resultphiTest = QueryExecutor::executeFunction(phiTest,args);
    std::cout << "test4: passed: " << resultphiTest.IntVal.getZExtValue() << "\n";
}

void executePhi5Test() {
    ModuleGen moduleGen("Phi5TestModule");
    std::vector<llvm::GenericValue> args;
    llvm::Function * phiTest = phi_5_test();
    llvm::GenericValue resultphiTest = QueryExecutor::executeFunction(phiTest,args);
    std::cout << "test5: passed: " << resultphiTest.IntVal.getZExtValue() << "\n";
}

void phi_test() {
    executePhi1Test();
    executePhi2Test();
    executePhi3Test();
    executePhi4Test();
    executePhi5Test();
}