//
// Created by josef on 06.01.17.
//

#include "queryCompiler/queryCompiler.hpp"

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
#include <include/tardisdb/sqlParser/SQLParser.hpp>

#include "algebra/logical/operators.hpp"
#include "algebra/translation.hpp"
#include "codegen/CodeGen.hpp"
#include "foundations/exceptions.hpp"
#include "foundations/loader.hpp"
#include "foundations/version_management.hpp"
#include "queryExecutor/queryExecutor.hpp"

#include "include/tardisdb/semanticAnalyser/SemanticAnalyser.hpp"
#include "sqlParser/ParserResult.hpp"

#if USE_HYRISE
    #include "SQLParser.h"
#endif

namespace QueryCompiler {

    static llvm::Function *compileQuery(const std::string &query, std::unique_ptr<Operator> &queryTree, QueryContext &queryContext) {
        auto &codeGen = getThreadLocalCodeGen();
        auto &llvmContext = codeGen.getLLVMContext();
        auto &moduleGen = codeGen.getCurrentModuleGen();

        // query function signature
        llvm::FunctionType *funcTy = llvm::TypeBuilder<void(int, void *), false>::get(llvmContext);

        llvm::Function *queryFunc;
        {
            FunctionGen funcGen(moduleGen, "query", funcTy);

            auto physicalTree = Algebra::translateToPhysicalTree(*queryTree,queryContext);
            physicalTree->produce();

            queryFunc = funcGen.getFunction();
        }

        return queryFunc;
    }

    void compileAndExecute(const std::string &query, Database &db, void *callbackFunction) {
        QueryContext queryContext(db);

        ModuleGen moduleGen("QueryModule");

#if USE_HYRISE
        hsql::SQLParser::parse(query, &queryContext.hyriseResult);
        if (!queryContext.hyriseResult.isValid()) throw std::runtime_error("invalid statement");
        QueryContext::convertToParserResult(queryContext.analyzingContext.parserResult,queryContext.hyriseResult);
#else
        tardisParser::SQLParser::parseStatement(queryContext.parsingContext, query);
        QueryContext::convertToParserResult(queryContext.analyzingContext.parserResult,queryContext.parsingContext);
#endif

        std::unique_ptr<semanticalAnalysis::SemanticAnalyser> analyser = semanticalAnalysis::SemanticAnalyser::getSemanticAnalyser(queryContext.analyzingContext);
        analyser->verify();
        analyser->constructTree();
        auto &queryTree = queryContext.analyzingContext.joinedTree;
        if (queryTree == nullptr) return;

        auto queryFunc = compileQuery(query, queryTree,queryContext);
        if (queryFunc == nullptr) return;

        QueryContext::constructBranchLineages(queryContext.analyzingContext.branchIds,queryContext);

        std::vector<llvm::GenericValue> args(2);
        args[0].IntVal = llvm::APInt(64, 5);
        args[1].PointerVal = (void *) &queryContext;

        QueryExecutor::executeFunction(queryFunc, args, callbackFunction);
    }

    BenchmarkResult compileAndBenchmark(const std::string &query, Database &db, void *callbackFunction) {
        QueryContext queryContext(db);

        ModuleGen moduleGen("QueryModule");

        const auto parsingStart = std::chrono::high_resolution_clock::now();
#if USE_HYRISE
        hsql::SQLParser::parse(query, &queryContext.hyriseResult);
        QueryContext::convertToParserResult(queryContext.analyzingContext.parserResult,queryContext.hyriseResult);
#else
        tardisParser::SQLParser::parseStatement(queryContext.parsingContext, query);
        QueryContext::convertToParserResult(queryContext.analyzingContext.parserResult,queryContext.parsingContext);
#endif
        const auto parsingDuration = std::chrono::high_resolution_clock::now() - parsingStart;

        const auto analysingStart = std::chrono::high_resolution_clock::now();
        std::unique_ptr<semanticalAnalysis::SemanticAnalyser> analyser = semanticalAnalysis::SemanticAnalyser::getSemanticAnalyser(queryContext.analyzingContext);
        analyser->verify();
        analyser->constructTree();
        auto &queryTree = queryContext.analyzingContext.joinedTree;
        const auto analysingDuration = std::chrono::high_resolution_clock::now() - analysingStart;
        if (queryTree == nullptr) return BenchmarkResult();
        
        // return columns
        BenchmarkResult result;
        result.columns = queryTree->getRoot()->getRequired();

        const auto translationStart = std::chrono::high_resolution_clock::now();
        auto queryFunc = compileQuery(query, queryTree, queryContext);
        const auto translationDuration = std::chrono::high_resolution_clock::now() - translationStart;
        if (queryFunc == nullptr) unreachable();

        QueryContext::constructBranchLineages(queryContext.analyzingContext.branchIds,queryContext);

        std::vector<llvm::GenericValue> args(2);
        args[0].IntVal = llvm::APInt(64, 5);
        args[1].PointerVal = (void *) &queryContext;

        QueryExecutor::BenchmarkResult llvmresult = QueryExecutor::executeBenchmarkFunction(queryFunc, args, 1, callbackFunction);

        result.parsingTime = std::chrono::duration_cast<std::chrono::microseconds>(parsingDuration).count();
        result.analysingTime = std::chrono::duration_cast<std::chrono::microseconds>(analysingDuration).count();
        result.translationTime = std::chrono::duration_cast<std::chrono::microseconds>(translationDuration).count();
        result.llvmCompilationTime = llvmresult.compilationTime;
        result.executionTime = llvmresult.executionTime;

        return result;
    }

} // end namespace QueryCompiler
