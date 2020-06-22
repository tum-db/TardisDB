//
// Created by josef on 29.12.16.
//

#include <llvm/IR/TypeBuilder.h>
#include <iostream>

#include "codegen/CodeGen.hpp"
#include "codegen/PhiNode.hpp"
#include "queryExecutor/queryExecutor.hpp"
#include "gtest/gtest.h"

llvm::Function * if_1_test()
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void (), false>::get(context);
    FunctionGen funcGen(moduleGen, "if1Test", funcTy);

    {
        IfGen check(funcGen, cg_bool_t(true), {{"index", cg_int_t(0)}});
        {
            check.setVar(0, cg_int_t(1));
        }
        check.EndIf();
        cg_int_t index(check.getResult(0));

        funcGen.setReturnValue(codeGen->CreateICmpEQ(index, cg_int_t(1)));
    }

    return funcGen.getFunction();
}

llvm::Function * if_2_test()
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void (), false>::get(context);
    FunctionGen funcGen(moduleGen, "if2Test", funcTy);

    {
        IfGen check( funcGen, cg_bool_t(false), {{"index", cg_int_t(0)}} );
        {
            check.setVar(0, cg_int_t(1));
        }
        check.EndIf();
        cg_int_t index(check.getResult(0));

        funcGen.setReturnValue(codeGen->CreateICmpEQ(index, cg_int_t(0)));
    }

    return funcGen.getFunction();
}

llvm::Function * if_3_test()
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void (), false>::get(context);
    FunctionGen funcGen(moduleGen, "if3Test", funcTy);

    {
        IfGen check( funcGen, cg_bool_t(true), {{"index", cg_int_t(0)}} );
        {
            check.setVar(0, cg_int_t(1));
        }
        check.Else();
        {
            check.setVar(0, cg_int_t(2));
        }
        check.EndIf();
        cg_int_t index(check.getResult(0));

        funcGen.setReturnValue(codeGen->CreateICmpEQ(index, cg_int_t(1)));
    }

    return funcGen.getFunction();
}

llvm::Function * if_4_test()
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void (), false>::get(context);
    FunctionGen funcGen(moduleGen, "if4Test", funcTy);

    {
        IfGen check( funcGen, cg_bool_t(false), {{"index", cg_int_t(0)}} );
        {
            check.setVar(0, cg_int_t(1));
        }
        check.Else();
        {
            check.setVar(0, cg_int_t(2));
        }
        check.EndIf();
        cg_int_t index(check.getResult(0));

        funcGen.setReturnValue(codeGen->CreateICmpEQ(index, cg_int_t(2)));
    }

    return funcGen.getFunction();
}

llvm::Function * if_5_test()
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void (), false>::get(context);
    FunctionGen funcGen(moduleGen, "if5Test", funcTy);

    {
        IfGen check( funcGen, cg_bool_t(false), {{"index", cg_int_t(0)}} );
        {
            check.setVar(0, cg_int_t(1));
        }
        check.ElseIf( cg_bool_t(true) );
        {
            check.setVar(0, cg_int_t(2));
        }
        check.Else();
        {
            check.setVar(0, cg_int_t(3));
        }
        check.EndIf();
        cg_int_t index(check.getResult(0));

        funcGen.setReturnValue(codeGen->CreateICmpEQ(index, cg_int_t(2)));
    }

    return funcGen.getFunction();
}

llvm::Function * if_6_test()
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void (), false>::get(context);
    FunctionGen funcGen(moduleGen, "if6Test", funcTy);

    {
        PhiNode<llvm::Value> node( cg_bool_t(false), "node" );
        IfGen check( funcGen, cg_bool_t(false), {{"index", cg_int_t(0)}} );
        {
            node = cg_bool_t(false);
        }
        check.ElseIf( cg_bool_t(true) );
        {
            node = cg_bool_t(true);
        }
        check.Else();
        {
            node = cg_bool_t(false);
        }
        check.EndIf();

        cg_bool_t result(node.get());
        funcGen.setReturnValue(result);
    }

    return funcGen.getFunction();
}

llvm::Function * if_7_test()
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void (), false>::get(context);
    FunctionGen funcGen(moduleGen, "if7Test", funcTy);

    {
        IfGen check1( funcGen, cg_bool_t(true), {{"index", cg_int_t(0)}} );
        {
            IfGen check2( funcGen, cg_bool_t(true), {{"index", cg_int_t(0)}} );
            {
                check2.setVar(0, cg_int_t(3));
            }
            check2.EndIf();

            check1.setVar(0, check2.getResult(0));
        }
        check1.EndIf();
        cg_int_t index(check1.getResult(0));

        funcGen.setReturnValue(codeGen->CreateICmpEQ(index, cg_int_t(3)));
    }

    return funcGen.getFunction();
}

llvm::Function * if_8_test()
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void (), false>::get(context);
    FunctionGen funcGen(moduleGen, "if8Test", funcTy);

    {
        PhiNode<Sql::Value> node( Sql::Bool(false), "node" );

        llvm::BasicBlock * passed = llvm::BasicBlock::Create(context, "passed");
        llvm::BasicBlock * exit = llvm::BasicBlock::Create(context, "exit");

        std::vector<std::unique_ptr<IfGen>> chain;

        unsigned last = 3;
        for (unsigned i = 0; i <= last; ++i) {
            // extend the "if"-chain by one statement
            chain.emplace_back( std::make_unique<IfGen>(funcGen, cg_bool_t(true)) );

            if (i == last) {
                codeGen.setNextBlock(passed);
            }
        }

        // close the chain in reverse order
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            it->get()->EndIf();
        }

        node = Sql::Bool(false);
        Functions::genPrintfCall("test8: passed: 0\n");
        codeGen->CreateBr(exit);

        llvm::Function * func = funcGen.getFunction();
        func->getBasicBlockList().push_back(passed);
        func->getBasicBlockList().push_back(exit);

        codeGen->SetInsertPoint(passed);
        node = Sql::Bool(true);
        Functions::genPrintfCall("test8: passed: 1\n");
        codeGen->CreateBr(exit);

        codeGen->SetInsertPoint(exit);

        Sql::value_op_t result = node.get();
        funcGen.setReturnValue(result->equals(Sql::Bool(true)));
    }

    return funcGen.getFunction();
}

TEST(IfTest, If1Test) {
    ModuleGen moduleGen("If1TestModule");
    std::vector<llvm::GenericValue> args;
    llvm::Function * ifTest = if_1_test();
    llvm::GenericValue resultifTest = QueryExecutor::executeFunction(ifTest,args);
    ASSERT_NE(resultifTest.IntVal.getZExtValue(),0);
}

TEST(IfTest, If2Test) {
    ModuleGen moduleGen("If2TestModule");
    std::vector<llvm::GenericValue> args;
    llvm::Function * ifTest = if_2_test();
    llvm::GenericValue resultifTest = QueryExecutor::executeFunction(ifTest,args);
    ASSERT_NE(resultifTest.IntVal.getZExtValue(),0);
}

TEST(IfTest, If3Test) {
    ModuleGen moduleGen("If3TestModule");
    std::vector<llvm::GenericValue> args;
    llvm::Function * ifTest = if_3_test();
    llvm::GenericValue resultifTest = QueryExecutor::executeFunction(ifTest,args);
    ASSERT_NE(resultifTest.IntVal.getZExtValue(),0);
}

TEST(IfTest, If4Test) {
    ModuleGen moduleGen("If4TestModule");
    std::vector<llvm::GenericValue> args;
    llvm::Function * ifTest = if_4_test();
    llvm::GenericValue resultifTest = QueryExecutor::executeFunction(ifTest,args);
    ASSERT_NE(resultifTest.IntVal.getZExtValue(),0);
}

TEST(IfTest, If5Test) {
    ModuleGen moduleGen("If5TestModule");
    std::vector<llvm::GenericValue> args;
    llvm::Function * ifTest = if_5_test();
    llvm::GenericValue resultifTest = QueryExecutor::executeFunction(ifTest,args);
    ASSERT_NE(resultifTest.IntVal.getZExtValue(),0);
}

TEST(IfTest, If6Test) {
    ModuleGen moduleGen("If6TestModule");
    std::vector<llvm::GenericValue> args;
    llvm::Function * ifTest = if_6_test();
    llvm::GenericValue resultifTest = QueryExecutor::executeFunction(ifTest,args);
    ASSERT_NE(resultifTest.IntVal.getZExtValue(),0);
}

TEST(IfTest, If7Test) {
    ModuleGen moduleGen("If7TestModule");
    std::vector<llvm::GenericValue> args;
    llvm::Function * ifTest = if_7_test();
    llvm::GenericValue resultifTest = QueryExecutor::executeFunction(ifTest,args);
    ASSERT_NE(resultifTest.IntVal.getZExtValue(),0);
}

TEST(IfTest, If8Test) {
    ModuleGen moduleGen("If8TestModule");
    std::vector<llvm::GenericValue> args;
    llvm::Function * ifTest = if_8_test();
    llvm::GenericValue resultifTest = QueryExecutor::executeFunction(ifTest,args);
    ASSERT_NE(resultifTest.IntVal.getZExtValue(),0);
}

void executeIf1Test() {
    ModuleGen moduleGen("If1TestModule");
    std::vector<llvm::GenericValue> args;
    llvm::Function * ifTest = if_1_test();
    llvm::GenericValue resultifTest = QueryExecutor::executeFunction(ifTest,args);
    std::cout << "test1: passed: " << resultifTest.IntVal.getZExtValue() << "\n";
}

void executeIf2Test() {
    ModuleGen moduleGen("If2TestModule");
    std::vector<llvm::GenericValue> args;
    llvm::Function * ifTest = if_2_test();
    llvm::GenericValue resultifTest = QueryExecutor::executeFunction(ifTest,args);
    std::cout << "test2: passed: " << resultifTest.IntVal.getZExtValue() << "\n";
}

void executeIf3Test() {
    ModuleGen moduleGen("If3TestModule");
    std::vector<llvm::GenericValue> args;
    llvm::Function * ifTest = if_3_test();
    llvm::GenericValue resultifTest = QueryExecutor::executeFunction(ifTest,args);
    std::cout << "test3: passed: " << resultifTest.IntVal.getZExtValue() << "\n";
}

void executeIf4Test() {
    ModuleGen moduleGen("If4TestModule");
    std::vector<llvm::GenericValue> args;
    llvm::Function * ifTest = if_4_test();
    llvm::GenericValue resultifTest = QueryExecutor::executeFunction(ifTest,args);
    std::cout << "test4: passed: " << resultifTest.IntVal.getZExtValue() << "\n";
}

void executeIf5Test() {
    ModuleGen moduleGen("If5TestModule");
    std::vector<llvm::GenericValue> args;
    llvm::Function * ifTest = if_5_test();
    llvm::GenericValue resultifTest = QueryExecutor::executeFunction(ifTest,args);
    std::cout << "test5: passed: " << resultifTest.IntVal.getZExtValue() << "\n";
}

void executeIf6Test() {
    ModuleGen moduleGen("If6TestModule");
    std::vector<llvm::GenericValue> args;
    llvm::Function * ifTest = if_6_test();
    llvm::GenericValue resultifTest = QueryExecutor::executeFunction(ifTest,args);
    std::cout << "test6: passed: " << resultifTest.IntVal.getZExtValue() << "\n";
}

void executeIf7Test() {
    ModuleGen moduleGen("If7TestModule");
    std::vector<llvm::GenericValue> args;
    llvm::Function * ifTest = if_7_test();
    llvm::GenericValue resultifTest = QueryExecutor::executeFunction(ifTest,args);
    std::cout << "test7: passed: " << resultifTest.IntVal.getZExtValue() << "\n";
}

void executeIf8Test() {
    ModuleGen moduleGen("If8TestModule");
    std::vector<llvm::GenericValue> args;
    llvm::Function * ifTest = if_8_test();
    llvm::GenericValue resultifTest = QueryExecutor::executeFunction(ifTest,args);
    std::cout << "test8: passed: " << resultifTest.IntVal.getZExtValue() << "\n";
}

void if_test() {
    executeIf1Test();
    executeIf2Test();
    executeIf3Test();
    executeIf4Test();
    executeIf5Test();
    executeIf6Test();
    executeIf7Test();
    executeIf8Test();
}