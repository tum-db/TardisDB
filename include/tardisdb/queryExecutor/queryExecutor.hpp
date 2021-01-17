//
// Created by Blum Thomas on 2020-06-18.
//

#ifndef PROTODB_QUERYEXECUTOR_HPP
#define PROTODB_QUERYEXECUTOR_HPP

#include <llvm/IR/TypeBuilder.h>
#include <llvm/ExecutionEngine/GenericValue.h>

namespace QueryExecutor {

    struct BenchmarkResult {
        double compilationTime;
        double executionTime;
    };


    llvm::GenericValue executeFunction(llvm::Function *queryFunc, std::vector<llvm::GenericValue> &args, void *callbackFunction = nullptr);
    BenchmarkResult executeBenchmarkFunction(llvm::Function *queryFunc, std::vector<llvm::GenericValue> &args, unsigned runs = 1, void *callbackFunction = nullptr);
}

#endif //PROTODB_QUERYEXECUTOR_HPP
