//
// Created by josef on 30.12.16.
//

#include <llvm/IR/TypeBuilder.h>

#include "codegen/CodeGen.hpp"

using namespace llvm;

llvm::Function * loop_test()
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void(), false>::get(context);
    FunctionGen funcGen(moduleGen, "loopTest", funcTy);

#if 0
    {
        LoopGen loopGen( funcGen, {{"index", cg_size_t(0ul)}} );
        cg_size_t i( loopGen.getLoopVar(0) );
        {
            LoopBodyGen bodyGen(loopGen);
            Functions::genPrintfCall("iteration: %lu\n", i);
        }
        cg_size_t nextIndex( i + 1ul );
        loopGen.loopDone( nextIndex < 3, {nextIndex} );
    }
#endif

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

        Functions::genPrintfCall("test1: passed: %d\n", codeGen->CreateICmpEQ(indexV, codeGen->getInt32(0)));
    }

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

        Functions::genPrintfCall("test2: passed: %d\n", codeGen->CreateICmpEQ(indexV, codeGen->getInt32(2)));
    }

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

        Functions::genPrintfCall("test3: passed: %d\n", i == 3);
    }

    return funcGen.getFunction();
}
