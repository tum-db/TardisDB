//
// Created by josef on 06.01.17.
//

#ifndef LLVM_PROTOTYPE_COMPILER_HPP
#define LLVM_PROTOTYPE_COMPILER_HPP

#include <llvm/IR/Function.h>
#include <llvm/ExecutionEngine/GenericValue.h>

namespace QueryCompiler {

struct BenchmarkResult {
    double compilationTime;
    double executionTime;
};

/// \brief Benchmarks the given SQL query
/// \returns The compilation and the average execution time of the query
BenchmarkResult benchmark(const std::string & query, unsigned runs);

BenchmarkResult benchmark(llvm::Function * queryFunction, unsigned runs);

BenchmarkResult benchmark(llvm::Function * queryFunction, const std::vector<llvm::GenericValue> & args, unsigned runs);

/// \brief Executes the given SQL query
void execute(const std::string & query);

void execute(llvm::Function * queryFunction);

void execute(llvm::Function * queryFunction, const std::vector<llvm::GenericValue> & args);

} // end namespace QueryCompiler

#endif //LLVM_PROTOTYPE_COMPILER_HPP
