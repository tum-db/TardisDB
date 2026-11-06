//
// Created by Blum Thomas on 2020-06-18.
//

#ifndef PROTODB_QUERYCOMPILER_HPP
#define PROTODB_QUERYCOMPILER_HPP

#include <functional>
#include <string>
#include <vector>

#include "foundations/Database.hpp"
#include "foundations/InformationUnit.hpp"
#include "sql/SqlType.hpp"

namespace Native { namespace Sql { class SqlTuple; } }


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

    /// What a statement executed by execute() returned
    struct StatementResult {
        std::vector<std::string> columnNames; // of the result tuples, in their order
        std::vector<Sql::SqlType> columnTypes;
        bool returnsRows = false;             // a query, even one without rows
        uint64_t rows = 0;                    // returned, or written by INSERT, UPDATE and DELETE
        std::string tag;                      // the statement, as PostgreSQL names it ("SELECT", "INSERT", ...)
        std::string notice;                   // a message for the client, e.g. of MERGE
    };

    /// Executes one statement on behalf of a session: relations without VERSION are read and written
    /// in \p branch, and every result tuple goes to \p sink (refilled for the next row; called by
    /// the workers of a parallel scan under the result latch). \p described receives the columns
    /// of the result before the first tuple.
    StatementResult execute(const std::string & query, Database & db, const std::string & branch,
                            const std::function<void(Native::Sql::SqlTuple *)> & sink,
                            const std::function<void(const StatementResult &)> & described = nullptr);

}

#endif //PROTODB_QUERYCOMPILER_HPP
