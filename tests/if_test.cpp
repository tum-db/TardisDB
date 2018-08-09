//
// Created by josef on 29.12.16.
//

#include <llvm/IR/TypeBuilder.h>

#include "CodeGen.hpp"

llvm::Function * if_test()
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void (), false>::get(context);
    FunctionGen funcGen(moduleGen, "ifTest", funcTy);

    {
        IfGen check(funcGen, cg_bool_t(true), {{"index", cg_int_t(0)}});
        {
            check.setVar(0, cg_int_t(1));
        }
        check.EndIf();
        cg_int_t index(check.getResult(0));
        Functions::genPrintfCall("test1: passed: %d\n", codeGen->CreateICmpEQ(index, cg_int_t(1)));
    }

    {
        IfGen check( funcGen, cg_bool_t(false), {{"index", cg_int_t(0)}} );
        {
            check.setVar(0, cg_int_t(1));
        }
        check.EndIf();
        cg_int_t index(check.getResult(0));
        Functions::genPrintfCall("test2: passed: %d\n", codeGen->CreateICmpEQ(index, cg_int_t(0)));
    }

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
        Functions::genPrintfCall("test3: passed: %d\n", codeGen->CreateICmpEQ(index, cg_int_t(1)));
    }

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
        Functions::genPrintfCall("test4: passed: %d\n", codeGen->CreateICmpEQ(index, cg_int_t(2)));
    }

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
        Functions::genPrintfCall("test5: passed: %d\n", codeGen->CreateICmpEQ(index, cg_int_t(2)));
    }

    {
        IfGen check( funcGen, cg_bool_t(false), {{"index", cg_int_t(0)}} );
        {
            Functions::genPrintfCall("test6: failed\n");
        }
        check.ElseIf( cg_bool_t(true) );
        {
            Functions::genPrintfCall("test6: passed: 1\n");
        }
        check.Else();
        {
            Functions::genPrintfCall("test6: failed\n");
        }
        check.EndIf();
    }

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
        Functions::genPrintfCall("test7: passed: %d\n", codeGen->CreateICmpEQ(index, cg_int_t(3)));
    }

    {
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

        Functions::genPrintfCall("test8: passed: 0\n");
        codeGen->CreateBr(exit);

        llvm::Function * func = funcGen.getFunction();
        func->getBasicBlockList().push_back(passed);
        func->getBasicBlockList().push_back(exit);

        codeGen->SetInsertPoint(passed);
        Functions::genPrintfCall("test8: passed: 1\n");
        codeGen->CreateBr(exit);

        codeGen->SetInsertPoint(exit);
    }

    return funcGen.getFunction();
}
