//
// Created by Blum Thomas on 2020-06-18.
//

#ifndef PROTODB_QUERYCOMPILER_HPP
#define PROTODB_QUERYCOMPILER_HPP

#include <string>

#include "foundations/Database.hpp"
#include "foundations/InformationUnit.hpp"


namespace QueryCompiler {

    struct BenchmarkResult {
        double parsingTime;
        double analysingTime;
        double translationTime;
        double llvmCompilationTime;
        double executionTime;
        iu_set_t columns;

        BenchmarkResult () : parsingTime(0), analysingTime(0), translationTime(0), llvmCompilationTime(0), executionTime(0) {}

        BenchmarkResult& operator+=(const BenchmarkResult& rhs) {
          parsingTime += rhs.parsingTime;
          analysingTime += rhs.analysingTime;
          translationTime += rhs.translationTime;
          llvmCompilationTime += rhs.llvmCompilationTime;
          executionTime += rhs.executionTime;
          columns = rhs.columns;
          return *this;
        }
    };

    /// Statements may run concurrently from several threads; each holds latches (see queryCompiler.cpp)
    enum class Latching {
        Serial, // one statement at a time
        Table,  // on the catalog and on the tables a statement accesses
        Branch, // in addition on the branches it accesses: updates and deletes in a branch other than
                // main exclude only the readers and writers of that branch (the default)
    };
    extern Latching latching;

    void compileAndExecute(const std::string & query, Database &db, void *callbackFunction = nullptr);

    BenchmarkResult compileAndBenchmark(const std::string & query, Database &db, void *callbackFunction = nullptr);

}

#endif //PROTODB_QUERYCOMPILER_HPP
