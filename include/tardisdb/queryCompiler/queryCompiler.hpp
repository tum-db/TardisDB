//
// Created by Blum Thomas on 2020-06-18.
//

#ifndef PROTODB_QUERYCOMPILER_HPP
#define PROTODB_QUERYCOMPILER_HPP

#include <functional>
#include <stdexcept>
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

    /// A transaction of a session over several statements (BEGIN ... COMMIT). It runs in a private branch,
    /// its snapshot, forked from the session's branch at BEGIN: its statements see the branch as it was
    /// then, with their own writes, and other statements do not see these writes. COMMIT merges the
    /// snapshot into the branch with MergePolicy::FirstCommitterWins and drops it; ROLLBACK drops it.
    /// This is snapshot isolation: of two transactions that write the same tuple, the second to commit
    /// fails, while two that each read what the other writes may both commit (write skew).
    struct Transaction {
        std::string branch;   // of the session, which the transaction commits into
        std::string snapshot; // the private branch, under a name that no statement can name
    };

    /// A statement that a transaction does not allow, or a commit that conflicts, with its SQLSTATE
    struct TransactionError : std::runtime_error {
        TransactionError(const std::string & sqlState, const std::string & message) : std::runtime_error(message), sqlState(sqlState) { }
        std::string sqlState;
    };

    /// BEGIN in \p branch. Like the statements that change the catalog, BEGIN, COMMIT and ROLLBACK hold
    /// the catalog latch exclusive: they create, merge and drop the snapshot.
    Transaction beginTransaction(Database & db, const std::string & branch);

    /// COMMIT. \throws TransactionError (40001) if a transaction that committed since the BEGIN of this
    /// one, or a statement outside of transactions, wrote a tuple this one wrote; nothing is committed then
    void commitTransaction(Database & db, const Transaction & transaction);

    /// ROLLBACK
    void rollbackTransaction(Database & db, const Transaction & transaction);

    /// Executes one statement on behalf of a session: relations without VERSION are read and written
    /// in \p branch, and every result tuple goes to \p sink (refilled for the next row; called by
    /// the workers of a parallel scan under the result latch). \p described receives the columns
    /// of the result before the first tuple. In a \p transaction, the statement reads and writes its
    /// snapshot in place of \p branch; it may read other branches (as they are when it runs), but it
    /// writes only the snapshot, and only SELECT, INSERT, UPDATE and DELETE run (TransactionError 25001).
    StatementResult execute(const std::string & query, Database & db, const std::string & branch,
                            const std::function<void(Native::Sql::SqlTuple *)> & sink,
                            const std::function<void(const StatementResult &)> & described = nullptr,
                            const Transaction * transaction = nullptr);

}

#endif //PROTODB_QUERYCOMPILER_HPP
