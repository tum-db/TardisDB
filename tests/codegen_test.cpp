//
// Created by josef on 30.12.16.
//

#include <llvm/IR/TypeBuilder.h>

#include "CodeGen.hpp"

llvm::Function * codegen_test()
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void(), false>::get(context);
    FunctionGen funcGen(moduleGen, "codegenTest", funcTy);

    using namespace TypeWrappers;

    Functions::genPrintfCall("types test:\n");
    Functions::genPrintfCall("printf test1\n");
    Functions::genPrintfCall("printf test1: arg: %d\n", codeGen->getInt32(1));

    // Types test:
    Int32 i = Int32(1) + Int32(1);
    Functions::genPrintfCall("1+1: %d\n", i);

    Int32 i2 = Int32(6) * Int32(7);
    Functions::genPrintfCall("6*7: %d\n", i2);

    Bool r = (Bool(true) == Bool(true));
    Functions::genPrintfCall("Bool(true) == Bool(true): %d\n", r);

    Bool r2 = (Bool(true) == Bool(false));
    Functions::genPrintfCall("Bool(true) == Bool(false): %d\n", r2);

    return funcGen.getFunction();
}
