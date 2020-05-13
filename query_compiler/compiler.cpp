//
// Created by josef on 06.01.17.
//

#include "query_compiler/compiler.hpp"

#include <chrono>

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

#include "algebra/logical/operators.hpp"
#include "algebra/translation.hpp"
#include "codegen/CodeGen.hpp"
#include "foundations/exceptions.hpp"
#include "foundations/loader.hpp"
#include "foundations/version_management.hpp"
#include "query_compiler/queryparser.hpp"
#include "query_compiler/semantic_analysis.hpp"

#include "semanticAnalysis/SemanticAnalyser.hpp"

using namespace std::chrono;

namespace QueryCompiler {

static high_resolution_clock::time_point compilationStart;

//#define EMIT_IR
//#define DISABLE_OPTIMIZATIONS

#ifndef DISABLE_OPTIMIZATIONS
/*
 * http://cs.swan.ac.uk/~csdavec/FOSDEM12/compiler.cc.html
 * http://the-ravi-programming-language.readthedocs.io/en/latest/llvm-notes.html
 * http://www.rubydoc.info/github/chriswailes/RCGTK/master/RCGTK/PassManager
 *
 * https://www.doc.ic.ac.uk/~dsl11/klee-doxygen/Optimize_8cpp_source.html
 * http://llvm.org/docs/WritingAnLLVMPass.html
 */
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

static void optimize(llvm::Module & module)
{
    llvm::legacy::PassManager pm;
    addPasses(pm);
    pm.run(module);
}
#endif // DISABLE_OPTIMIZATIONS

static llvm::Function * compileQuery(const std::string & query, QueryContext & queryContext)
{
//    throw NotImplementedException("compileQuery(const std::string & query)");

#if 1


#if 1
    auto queryTree = SemanticAnalyser::parse_and_construct_tree(queryContext, query);
#else
    auto parsedQuery = QueryParser::parse_query(query);
    auto queryTree = computeTree(*parsedQuery.get(), queryContext);
#endif

    auto & codeGen = getThreadLocalCodeGen();
    auto & llvmContext = codeGen.getLLVMContext();
    auto & moduleGen = codeGen.getCurrentModuleGen();

    // query function signature
    llvm::FunctionType * funcTy = llvm::TypeBuilder<void (), false>::get(llvmContext);

    // generate the query function
    llvm::Function * queryFunc;
    {
        FunctionGen funcGen(moduleGen, "query", funcTy);

        auto physicalTree = Algebra::translateToPhysicalTree(*queryTree);
        physicalTree->produce();

        queryFunc = funcGen.getFunction();
    }

    auto & module = moduleGen.getModule();

#ifdef EMIT_IR
    llvm::outs() << GRN << "We just constructed this LLVM module:\n\n" << RESET << module;
    llvm::outs().flush();
#endif

#ifndef DISABLE_OPTIMIZATIONS
    optimize(module);

#ifdef EMIT_IR
    llvm::outs() << GRN << "\nOptimized module:\n\n" << RESET << module;
    llvm::outs().flush();
#endif

#endif // DISABLE_OPTIMIZATIONS

    return queryFunc;
#endif
}

static void executeQueryFunction(llvm::Function * queryFunc)
{
    auto & moduleGen = getThreadLocalCodeGen().getCurrentModuleGen();

    llvm::EngineBuilder eb( moduleGen.finalizeModule() );
    eb.setOptLevel( llvm::CodeGenOpt::None );
//    eb.setOptLevel( llvm::CodeGenOpt::Less );
//    eb.setOptLevel( llvm::CodeGenOpt::Default );
//    eb.setOptLevel( llvm::CodeGenOpt::Aggressive );

    auto ee = std::unique_ptr<llvm::ExecutionEngine>( eb.create() );
    ee->finalizeObject();
    const auto compilationDuration = high_resolution_clock::now() - compilationStart;

    std::vector<llvm::GenericValue> noargs;

    const auto query_start = high_resolution_clock::now();
    ee->runFunction(queryFunc, noargs);
    const auto queryDuration = high_resolution_clock::now() - query_start;

    printf("Compilation time: %lums\n", duration_cast<milliseconds>(compilationDuration).count());
    printf("Execution time: %lums\n", duration_cast<milliseconds>(queryDuration).count());
}

void compileAndExecute(const std::string & query)
{
//    ModuleGen moduleGen("QueryModule");

//    auto db = loadUniDb();
    auto db = loadDatabase();
    QueryContext queryContext(*db); // TODO as function argument?

    compilationStart = high_resolution_clock::now();

    ModuleGen moduleGen("QueryModule");
    auto queryFunc = compileQuery(query, queryContext);

    executeQueryFunction(queryFunc);
}

void compileAndExecute(llvm::Function * queryFunction)
{
    auto & codeGen = getThreadLocalCodeGen();
    assert(codeGen.hasModuleGen());

    auto & module = codeGen.getCurrentModule();

#ifdef EMIT_IR
    llvm::outs() << GRN << "We just constructed this LLVM module:\n\n" << RESET << module;
    llvm::outs().flush();
#endif

#ifndef DISABLE_OPTIMIZATIONS
    optimize(module);

#ifdef EMIT_IR
    llvm::outs() << GRN << "\nOptimized module:\n\n" << RESET << module;
    llvm::outs().flush();
#endif

#endif // DISABLE_OPTIMIZATIONS

    compilationStart = high_resolution_clock::now();

    executeQueryFunction(queryFunction);
}

void compileAndExecute(llvm::Function * queryFunction, const std::vector<llvm::GenericValue> & args)
{
    throw NotImplementedException();
}

/// \return A pair with the compilation time in microseconds as first element and the execution time as second
static BenchmarkResult benchmarkQueryFunction(llvm::Function * queryFunc, unsigned runs)
{
    auto & moduleGen = getThreadLocalCodeGen().getCurrentModuleGen();

    llvm::EngineBuilder eb( moduleGen.finalizeModule() );
//    eb.setOptLevel( llvm::CodeGenOpt::None );
//    eb.setOptLevel( llvm::CodeGenOpt::Less );
    eb.setOptLevel( llvm::CodeGenOpt::Default );
//    eb.setOptLevel( llvm::CodeGenOpt::Aggressive );

    auto ee = std::unique_ptr<llvm::ExecutionEngine>( eb.create() );
    ee->finalizeObject();
    const auto compilationDuration = high_resolution_clock::now() - compilationStart;

    std::vector<llvm::GenericValue> noargs;

    long timeAcc = 0;
    for (unsigned i = 0; i < runs; ++i) {
        const auto query_start = high_resolution_clock::now();
        ee->runFunction(queryFunc, noargs);
        const auto queryDuration = high_resolution_clock::now() - query_start;

        timeAcc += duration_cast<microseconds>(queryDuration).count();
    }

    BenchmarkResult result;
    result.compilationTime = duration_cast<microseconds>(compilationDuration).count();
    result.executionTime = double(timeAcc) / runs;
    return result;
}

BenchmarkResult compileAndBenchmark(const std::string & query, unsigned runs)
{
    /*
    ModuleGen moduleGen("QueryModule");

    compilationStart = high_resolution_clock::now();
    auto queryFunc = compileQuery(query);

    return benchmarkQueryFunction(queryFunc, runs);*/
    throw NotImplementedException();
}

BenchmarkResult compileAndBenchmark(llvm::Function * queryFunction, unsigned runs)
{
    auto & codeGen = getThreadLocalCodeGen();
    assert(codeGen.hasModuleGen());

    auto & module = codeGen.getCurrentModule();

#ifdef EMIT_IR
    llvm::outs() << GRN << "We just constructed this LLVM module:\n\n" << RESET << module;
    llvm::outs().flush();
#endif

#ifndef DISABLE_OPTIMIZATIONS
    optimize(module);

#ifdef EMIT_IR
    llvm::outs() << GRN << "\nOptimized module:\n\n" << RESET << module;
    llvm::outs().flush();
#endif

#endif // DISABLE_OPTIMIZATIONS

    compilationStart = high_resolution_clock::now();

    return benchmarkQueryFunction(queryFunction, runs);
}

BenchmarkResult compileAndBenchmark(llvm::Function * queryFunction, const std::vector<llvm::GenericValue> & args, unsigned runs)
{
    throw NotImplementedException();
}

} // end namespace QueryCompiler
