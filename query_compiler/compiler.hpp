//
// Created by josef on 06.01.17.
//

#ifndef LLVM_PROTOTYPE_COMPILER_HPP
#define LLVM_PROTOTYPE_COMPILER_HPP

#include <llvm/IR/Function.h>
#include <llvm/ExecutionEngine/GenericValue.h>
#include "foundations/Database.hpp"

namespace QueryCompiler {

struct BenchmarkResult {
    double compilationTime;
    double executionTime;
};

/// \brief Benchmarks the given SQL query
/// \returns The compilation and the average execution time of the query
BenchmarkResult compileAndBenchmark(const std::string & query, unsigned runs);

BenchmarkResult compileAndBenchmark(llvm::Function * queryFunction, unsigned runs);

BenchmarkResult compileAndBenchmark(llvm::Function * queryFunction, const std::vector<llvm::GenericValue> & args, unsigned runs);

/// \brief Executes the given SQL query
void compileAndExecute(const std::string & query, Database &db);

void compileAndExecute(llvm::Function * queryFunction, std::vector<llvm::GenericValue> &args);

llvm::GenericValue compileAndExecuteReturn(llvm::Function * queryFunction, std::vector<llvm::GenericValue> &args);

void compileAndExecute(llvm::Function * queryFunction, const std::vector<llvm::GenericValue> & args);

} // end namespace QueryCompiler

#endif //LLVM_PROTOTYPE_COMPILER_HPP
