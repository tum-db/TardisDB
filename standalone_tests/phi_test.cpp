
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

#include "codegen/CodeGen.hpp"
#include "codegen/PhiNode.hpp"
#include "utils/general.hpp"

/* TODO
#undef DISABLE_OPTIMIZATIONS
#define DISABLE_OPTIMIZATIONS
*/

using namespace llvm;

static llvm::Function * genTestFunc()
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    llvm::FunctionType * funcTy = llvm::TypeBuilder<void (), false>::get(context);
    FunctionGen funcGen(moduleGen, "phiTest", funcTy);

    {
        PhiNode<llvm::Value> node( codeGen->getInt32(42), "node" );
        IfGen check(funcGen, cg_bool_t(true));
        {
            node = codeGen->getInt32(7);
        }
        check.EndIf();

        cg_i32_t result( node.get() );
        Functions::genPrintfCall("test1: passed: %d\n", codeGen->CreateICmpEQ(result, cg_i32_t(7)));
    }

    {
        PhiNode<llvm::Value> node( codeGen->getInt32(42), "node" );
        IfGen check(funcGen, cg_bool_t(false));
        {
            node = codeGen->getInt32(7);
        }
        check.EndIf();

        cg_i32_t result( node.get() );
        Functions::genPrintfCall("test2: passed: %d\n", codeGen->CreateICmpEQ(result, cg_i32_t(42)));
    }

    {
        PhiNode<llvm::Value> node( codeGen->getInt32(42), "node" );
        IfGen check(funcGen, cg_bool_t(false));
        {
            node = codeGen->getInt32(7);
        }
        check.EndIf();

        // test instruction reodering
        Functions::genPrintfCall("additional instruction\n");

        cg_i32_t result( node.get() );
        Functions::genPrintfCall("test3: passed: %d\n", codeGen->CreateICmpEQ(result, cg_i32_t(42)));
    }

    // FIXME doesn't work with optimizations turned on
    {
        PhiNode<llvm::Value> node( codeGen->getInt32(0), "node" );
        IfGen check(funcGen, cg_bool_t(false));
        {
//            Functions::genPrintfCall("side effect1\n");
            node = codeGen->getInt32(7);
        }
        check.Else();
        {
//            Functions::genPrintfCall("side effect2\n");
            node = codeGen->getInt32(42);
        }
        check.EndIf();

        // test instruction reodering
        Functions::genPrintfCall("additional instruction\n");

        cg_i32_t result( node.get() );
        Functions::genPrintfCall("test4: passed: %d\n", codeGen->CreateICmpEQ(result, cg_i32_t(42)));
    }

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
        Functions::genPrintfCall("test5: passed: %d\n", codeGen->CreateICmpEQ(result->getLLVMValue(), cg_i32_t(7)));
    }

    // TODO more tests

    return funcGen.getFunction();
}

static void addPasses(llvm::legacy::PassManager & pm)
{
    using namespace llvm;
    using namespace llvm::legacy;

    pm.add(createInstructionCombiningPass());
    pm.add(createReassociatePass());
    pm.add(createGVNPass(false)); // there should be no redundant loads
    pm.add(createCFGSimplificationPass());
    pm.add(createAggressiveDCEPass());
    pm.add(createCFGSimplificationPass());
}

// TODO
static void optimize(llvm::Module & module)
{
    llvm::legacy::PassManager pm;
    addPasses(pm);
    pm.run(module);
}

/*
static llvm::Function * compileTest()
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getContext();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    // query function signature
    llvm::FunctionType * funcTy = llvm::TypeBuilder<void (), false>::get(context);

    // generate the query function
    llvm::Function * func;
    {
        FunctionGen funcGen(moduleGen, "query", funcTy);

        genTest();

        func = funcGen.getFunction();
    }

    return func;
}
*/

static void executeFunction(llvm::Function * func)
{
    auto & moduleGen = getThreadLocalCodeGen().getCurrentModuleGen();

    llvm::EngineBuilder eb( moduleGen.finalizeModule() );
    eb.setOptLevel( llvm::CodeGenOpt::None );
//    eb.setOptLevel( llvm::CodeGenOpt::Less );
//    eb.setOptLevel( llvm::CodeGenOpt::Default );
//    eb.setOptLevel( llvm::CodeGenOpt::Aggressive );

    auto ee = std::unique_ptr<llvm::ExecutionEngine>( eb.create() );
    ee->finalizeObject();


    std::vector<llvm::GenericValue> noargs;
    ee->runFunction(func, noargs);
}

void execute()
{
    ModuleGen moduleGen("TestModule");

    auto func = genTestFunc();

    auto & module = moduleGen.getModule();

#ifndef NDEBUG
    llvm::outs() << GRN << "We just constructed this LLVM module:\n\n" << RESET << module;
    llvm::outs().flush();
#endif

#ifndef DISABLE_OPTIMIZATIONS
    optimize(module);

#ifndef NDEBUG
    llvm::outs() << GRN << "\nOptimized module:\n\n" << RESET << module;
    llvm::outs().flush();
#endif

#endif // DISABLE_OPTIMIZATIONS



    executeFunction(func);
}

int main(int argc, char * argv[])
{
    // necessary for the LLVM JIT compiler
    InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    execute();

    llvm_shutdown();
    return 0;
}
