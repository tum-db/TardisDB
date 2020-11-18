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

    void compileAndExecute(const std::string & query, Database &db, void *callbackFunction = nullptr);

    BenchmarkResult compileAndBenchmark(const std::string & query, Database &db, void *callbackFunction = nullptr);

}

#endif //PROTODB_QUERYCOMPILER_HPP
