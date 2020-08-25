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

#include "algebra/logical/operators.hpp"
#include "algebra/translation.hpp"
#include "codegen/CodeGen.hpp"
#include "foundations/exceptions.hpp"
#include "foundations/loader.hpp"
#include "foundations/version_management.hpp"
#include "queryExecutor/queryExecutor.hpp"

#include "include/tardisdb/semanticAnalyser/SemanticAnalyser.hpp"

namespace QueryCompiler {

    static llvm::Function *compileQuery(const std::string &query, std::unique_ptr<Operator> &queryTree) {
        auto &codeGen = getThreadLocalCodeGen();
        auto &llvmContext = codeGen.getLLVMContext();
        auto &moduleGen = codeGen.getCurrentModuleGen();

        // query function signature
        llvm::FunctionType *funcTy = llvm::TypeBuilder<void(int, void *), false>::get(llvmContext);

        llvm::Function *queryFunc;
        {
            FunctionGen funcGen(moduleGen, "query", funcTy);

            auto physicalTree = Algebra::translateToPhysicalTree(*queryTree);
            physicalTree->produce();

            queryFunc = funcGen.getFunction();
        }

        return queryFunc;
    }


    void compileAndExecute(const std::string &query, Database &db, void *callbackFunction) {
        QueryContext queryContext(db);

        ModuleGen moduleGen("QueryModule");

        tardisParser::SQLParserResult parserResult = tardisParser::SQLParser::parse_sql_statement(query);

        std::unique_ptr<semanticalAnalysis::SemanticAnalyser> analyser = semanticalAnalysis::SemanticAnalyser::getSemanticAnalyser(queryContext,parserResult);
        analyser->verify();
        auto queryTree = analyser->constructTree();
        if (queryTree == nullptr) return;

        auto queryFunc = compileQuery(query, queryTree);
        if (queryFunc == nullptr) return;

        std::vector<llvm::GenericValue> args(2);
        args[0].IntVal = llvm::APInt(64, 5);
        args[1].PointerVal = (void *) &queryContext;

        QueryExecutor::executeFunction(queryFunc, args, callbackFunction);
    }

    BenchmarkResult compileAndBenchmark(const std::string &query, Database &db) {
        QueryContext queryContext(db);

        ModuleGen moduleGen("QueryModule");

        const auto parsingStart = std::chrono::high_resolution_clock::now();
        tardisParser::SQLParserResult parserResult = tardisParser::SQLParser::parse_sql_statement(query);
        const auto parsingDuration = std::chrono::high_resolution_clock::now() - parsingStart;

        const auto analysingStart = std::chrono::high_resolution_clock::now();
        std::unique_ptr<semanticalAnalysis::SemanticAnalyser> analyser = semanticalAnalysis::SemanticAnalyser::getSemanticAnalyser(queryContext,parserResult);
        analyser->verify();
        auto queryTree = analyser->constructTree();
        const auto analysingDuration = std::chrono::high_resolution_clock::now() - analysingStart;
        if (queryTree == nullptr) unreachable();

        const auto translationStart = std::chrono::high_resolution_clock::now();
        auto queryFunc = compileQuery(query, queryTree);
        const auto translationDuration = std::chrono::high_resolution_clock::now() - translationStart;
        if (queryFunc == nullptr) unreachable();

        std::vector<llvm::GenericValue> args(2);
        args[0].IntVal = llvm::APInt(64, 5);
        args[1].PointerVal = (void *) &queryContext;

        QueryExecutor::BenchmarkResult llvmresult = QueryExecutor::executeBenchmarkFunction(queryFunc, args);

        BenchmarkResult result;
        result.parsingTime = std::chrono::duration_cast<std::chrono::microseconds>(parsingDuration).count();
        result.analysingTime = std::chrono::duration_cast<std::chrono::microseconds>(analysingDuration).count();
        result.translationTime = std::chrono::duration_cast<std::chrono::microseconds>(translationDuration).count();
        result.llvmCompilationTime = llvmresult.compilationTime;
        result.executionTime = llvmresult.executionTime;

        return result;
    }

} // end namespace QueryCompiler
