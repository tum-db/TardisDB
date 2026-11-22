//
// Created by Blum Thomas on 2020-06-18.
//

#ifndef PROTODB_QUERYEXECUTOR_HPP
#define PROTODB_QUERYEXECUTOR_HPP

#include "codegen/LLVMCompat.hpp"
#include <llvm/ExecutionEngine/GenericValue.h>

namespace QueryExecutor {

    struct BenchmarkResult {
        double compilationTime;
        double executionTime;
    };


    /// The times of the calls of executeFunction() on this thread, in milliseconds: compilation (the
    /// optimisation passes and the machine code) and execution of the generated code
    struct RunTimes {
        double compilation = 0;
        double execution = 0;
    };
    RunTimes & runTimes();

    llvm::GenericValue executeFunction(llvm::Function *queryFunc, std::vector<llvm::GenericValue> &args, void *callbackFunction = nullptr);
    BenchmarkResult executeBenchmarkFunction(llvm::Function *queryFunc, std::vector<llvm::GenericValue> &args, unsigned runs = 1, void *callbackFunction = nullptr);
}

#endif //PROTODB_QUERYEXECUTOR_HPP
