//
// Created by josef on 06.01.17.
//

#include "queryExecutor/queryExecutor.hpp"

#include <chrono>

#include <llvm/Support/raw_ostream.h>
#include "codegen/LLVMCompat.hpp"
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

#include "algebra/logical/operators.hpp"
#include "algebra/translation.hpp"
#include "codegen/CodeGen.hpp"
#include "foundations/exceptions.hpp"
#include "foundations/loader.hpp"
#include "foundations/version_management.hpp"

#include "include/tardisdb/semanticAnalyser/SemanticAnalyser.hpp"

//#define EMIT_IR
//#define DISABLE_OPTIMIZATIONS
//#define EMIT_RUNTIME


namespace QueryExecutor {

#ifndef DISABLE_OPTIMIZATIONS

/*
 * http://cs.swan.ac.uk/~csdavec/FOSDEM12/compiler.cc.html
 * http://the-ravi-programming-language.readthedocs.io/en/latest/llvm-notes.html
 * http://www.rubydoc.info/github/chriswailes/RCGTK/master/RCGTK/PassManager
 *
 * https://www.doc.ic.ac.uk/~dsl11/klee-doxygen/Optimize_8cpp_source.html
 * http://llvm.org/docs/WritingAnLLVMPass.html
 */
    static void addPasses(llvm::legacy::PassManager &pm) {
        using namespace llvm;
        using namespace llvm::legacy;

        //pm.add(createInstructionCombiningPass());
        pm.add(createReassociatePass());
        pm.add(createGVNPass(false)); // there should be no redundant loads
        //pm.add(createCFGSimplificationPass());
#if LLVM_VERSION_MAJOR < 18
        pm.add(createAggressiveDCEPass());
#else
        pm.add(createDeadCodeEliminationPass()); // the legacy pass manager lost aggressive DCE
#endif
    }

    static void optimize(llvm::Module &module) {
        llvm::legacy::PassManager pm;
        addPasses(pm);
        pm.run(module);
    }

#endif // DISABLE_OPTIMIZATIONS

    void defaultStreamingCallback(Native::Sql::SqlTuple *tuple) {}

    llvm::GenericValue executeFunction(llvm::Function *queryFunc, std::vector<llvm::GenericValue> &args, void *callbackFunction) {
        auto &moduleGen = getThreadLocalCodeGen().getCurrentModuleGen();

        llvm::FunctionType * funcUpdateTupleTy = llvm::TypeBuilder<void (void *), false>::get(getThreadLocalCodeGen().getLLVMContext());
        llvm::Function * func = LLVMCompat::getOrInsertFunction(getThreadLocalCodeGen().getCurrentModuleGen().getModule(), "callHandler", funcUpdateTupleTy);
        moduleGen.addFunctionMapping(func,(callbackFunction == nullptr ? (void*) defaultStreamingCallback : callbackFunction));

#ifdef EMIT_IR
        llvm::outs() << GRN << "We just constructed this LLVM module:\n\n" << RESET << moduleGen.getModule();
        llvm::outs().flush();
#endif

        // TARDISDB_VERIFY_IR=1 checks the generated code before it is compiled
        if (std::getenv("TARDISDB_VERIFY_IR") != nullptr && llvm::verifyModule(moduleGen.getModule(), &llvm::errs())) {
            llvm::errs() << moduleGen.getModule();
            throw std::runtime_error("invalid IR");
        }

#ifndef DISABLE_OPTIMIZATIONS
        optimize(moduleGen.getModule());

#ifdef EMIT_IR
        llvm::outs() << GRN << "\nOptimized module:\n\n" << RESET << moduleGen.getModule();
        llvm::outs().flush();
#endif

#endif // DISABLE_OPTIMIZATIONS

        const auto compilationStart = std::chrono::high_resolution_clock::now();

        llvm::EngineBuilder eb(moduleGen.finalizeModule());
        eb.setOptLevel(LLVMCompat::codeGenOptNone);
//    eb.setOptLevel( llvm::CodeGenOpt::Less );
//    eb.setOptLevel( llvm::CodeGenOpt::Default );
//    eb.setOptLevel( llvm::CodeGenOpt::Aggressive );
        auto ee = std::unique_ptr<llvm::ExecutionEngine>(eb.create());
        moduleGen.applyMapping(ee.get());
        ee->finalizeObject();

        const auto compilationDuration = std::chrono::high_resolution_clock::now() - compilationStart;


        const auto query_start = std::chrono::high_resolution_clock::now();
        llvm::GenericValue returnValue = ee->runFunction(queryFunc, args);
        const auto queryDuration = std::chrono::high_resolution_clock::now() - query_start;

#ifdef EMIT_RUNTIME
        printf("Compilation time: %lums\n", std::chrono::duration_cast<std::chrono::milliseconds>(compilationDuration).count());
        printf("Execution time: %lums\n", std::chrono::duration_cast<std::chrono::milliseconds>(queryDuration).count());
#endif

        return returnValue;
    }

/// \return A pair with the compilation time in microseconds as first element and the execution time as second
    BenchmarkResult executeBenchmarkFunction(llvm::Function *queryFunc, std::vector<llvm::GenericValue> &args, unsigned runs, void *callbackFunction) {
        auto &moduleGen = getThreadLocalCodeGen().getCurrentModuleGen();

        llvm::FunctionType * funcUpdateTupleTy = llvm::TypeBuilder<void (void *), false>::get(getThreadLocalCodeGen().getLLVMContext());
        llvm::Function * func = LLVMCompat::getOrInsertFunction(getThreadLocalCodeGen().getCurrentModuleGen().getModule(), "callHandler", funcUpdateTupleTy);
        moduleGen.addFunctionMapping(func,(callbackFunction == nullptr ? (void*) defaultStreamingCallback : callbackFunction));

#ifdef EMIT_IR
        llvm::outs() << GRN << "We just constructed this LLVM module:\n\n" << RESET << moduleGen.getModule();
        llvm::outs().flush();
#endif

#ifndef DISABLE_OPTIMIZATIONS
        optimize(moduleGen.getModule());

#ifdef EMIT_IR
        llvm::outs() << GRN << "\nOptimized module:\n\n" << RESET << moduleGen.getModule();
        llvm::outs().flush();
#endif

#endif // DISABLE_OPTIMIZATIONS

        const auto compilationStart = std::chrono::high_resolution_clock::now();

        llvm::EngineBuilder eb(moduleGen.finalizeModule());
         eb.setOptLevel( LLVMCompat::codeGenOptNone );
        // eb.setOptLevel( llvm::CodeGenOpt::Less );
        // eb.setOptLevel(llvm::CodeGenOpt::Default);
        // eb.setOptLevel( llvm::CodeGenOpt::Aggressive );
        auto ee = std::unique_ptr<llvm::ExecutionEngine>(eb.create());
        moduleGen.applyMapping(ee.get());
        ee->finalizeObject();

        const auto compilationDuration = std::chrono::high_resolution_clock::now() - compilationStart;

        long timeAcc = 0;
        for (unsigned i = 0; i < runs; ++i) {
            const auto query_start = std::chrono::high_resolution_clock::now();
            ee->runFunction(queryFunc, args);
            const auto queryDuration = std::chrono::high_resolution_clock::now() - query_start;

            timeAcc += std::chrono::duration_cast<std::chrono::microseconds>(queryDuration).count();
        }

        BenchmarkResult result;
        result.compilationTime = std::chrono::duration_cast<std::chrono::microseconds>(compilationDuration).count();
        result.executionTime = double(timeAcc) / runs;

        return result;
    }

} // end namespace QueryExecutor
