//
// Created by josef on 30.12.16.
//

#include "codegen/LLVMCompat.hpp"

#include "codegen/CodeGen.hpp"
#include "foundations/Database.hpp"
#include "queryExecutor/queryExecutor.hpp"

#include <sstream>
#include <vector>
#include <iostream>
#include <fstream>
#include <set>
#include "gtest/gtest.h"

using namespace llvm;

llvm::Function * loop_break_test()
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void(), false>::get(context);
    FunctionGen funcGen(moduleGen, "loopBreakTest", funcTy);

    // Loop break test:
    {
        Value * start = codeGen->getInt32(0);
        Value * limit = codeGen->getInt32(3);
        LoopGen loopGen(funcGen, codeGen->CreateICmpSLT(codeGen->getInt32(0), limit), {{"index", start}});
        Value * indexV = loopGen.getLoopVar(0);
        {
            LoopBodyGen bodyGen(loopGen);

            {
                IfGen check(funcGen, codeGen->getInt1(true));
                {
                    loopGen.loopBreak();
                }
            }
        }
        Value * nextIndex = codeGen->CreateAdd(indexV, codeGen->getInt32(1));
        loopGen.loopDone(codeGen->CreateICmpSLT(nextIndex, limit), {nextIndex});

        funcGen.setReturnValue(codeGen->CreateICmpEQ(indexV, codeGen->getInt32(0)));

    }

    return funcGen.getFunction();
}

llvm::Function * loop_continue_test()
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void(), false>::get(context);
    FunctionGen funcGen(moduleGen, "loopContinueTest", funcTy);

    // Loop continue test:
    {
        Value * start = codeGen->getInt32(0);
        Value * limit = codeGen->getInt32(3);
        LoopGen loopGen(funcGen, codeGen->CreateICmpSLT(codeGen->getInt32(0), limit), {{"index", start}});
        Value * indexV = loopGen.getLoopVar(0);
        {
            LoopBodyGen bodyGen(loopGen);

            {
                IfGen check(funcGen, codeGen->getInt1(true));
                {
                    loopGen.loopContinue();
                }
            }
        }
        Value * nextIndex = codeGen->CreateAdd(indexV, codeGen->getInt32(1));
        loopGen.loopDone(codeGen->CreateICmpSLT(nextIndex, limit), {nextIndex});

        funcGen.setReturnValue(codeGen->CreateICmpEQ(indexV, codeGen->getInt32(2)));

    }

    return funcGen.getFunction();
}

llvm::Function * loop_while_test()
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void(), false>::get(context);
    FunctionGen funcGen(moduleGen, "loopWhileTest", funcTy);

    // while(true) test:
    {
#ifdef __APPLE__
        LoopGen loopGen(funcGen, {{"index", cg_size_t(0ull)}});
#else
        LoopGen loopGen(funcGen, {{"index", cg_size_t(0ul)}});
#endif
        cg_size_t i( loopGen.getLoopVar(0) );
        {
            LoopBodyGen bodyGen(loopGen);

//            Functions::genPrintfCall("iteration: %lu\n", i);

            IfGen check(funcGen, i == 3);
            {
                loopGen.loopBreak();
            }
            check.EndIf();
        }
        cg_size_t nextIndex(i + 1ul);
        loopGen.loopDone(cg_bool_t(true), {nextIndex});

        funcGen.setReturnValue(i == 3);

    }

    return funcGen.getFunction();
}

TEST(LoopTest, LoopBreakTest) {
    ModuleGen moduleGen("LoopBreakTestModule");
    llvm::Function * loopBreakTest = loop_break_test();
    std::vector<llvm::GenericValue> args;
    llvm::GenericValue result = QueryExecutor::executeFunction(loopBreakTest,args);
    ASSERT_NE(result.IntVal.getZExtValue(),0);
}

TEST(LoopTest, LoopContinueTest) {
    ModuleGen moduleGen("LoopContinueTestModule");
    llvm::Function * loopBreakTest = loop_continue_test();
    std::vector<llvm::GenericValue> args;
    llvm::GenericValue result = QueryExecutor::executeFunction(loopBreakTest,args);
    ASSERT_EQ(result.IntVal.getZExtValue(),1);
}

TEST(LoopTest, LoopWhileTest) {
    ModuleGen moduleGen("LoopWhileTestModule");
    llvm::Function * loopBreakTest = loop_while_test();
    std::vector<llvm::GenericValue> args;
    llvm::GenericValue result = QueryExecutor::executeFunction(loopBreakTest,args);
    ASSERT_EQ(result.IntVal.getZExtValue(),1);
}
