// Transactions over several statements (QueryCompiler::Transaction): a private branch forked at BEGIN,
// merged at COMMIT with first-committer-wins and dropped at ROLLBACK. Snapshot reads, own writes, no
// lost updates under concurrent increments, and which statements a transaction confines or refuses

#include "codegen/LLVMCompat.hpp"
#include <algorithm>
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include "foundations/Parallel.hpp"
#include "runtime/SqlTuple.hpp"
#include "queryCompiler/queryCompiler.hpp"
#include <gtest/gtest.h>

namespace {

    using QueryCompiler::Transaction;
    using QueryCompiler::TransactionError;

    class TransactionTest : public ::testing::Test {
    protected:
        void SetUp() override {
            db = std::make_unique<Database>();
            exec("create table t ( k integer not null , x integer not null );");
            for (int k = 1; k <= 3; ++k) {
                exec("insert into t ( k , x ) values ( " + std::to_string(k) + " , " + std::to_string(10 * k) + " );");
            }
        }

        void exec(const std::string & sql, const Transaction * transaction = nullptr, const std::string & branch = "main") {
            QueryCompiler::execute(sql, *db, branch, [](Native::Sql::SqlTuple *) { }, nullptr, transaction);
        }

        /// \returns The rows of the query, sorted
        std::vector<std::string> query(const std::string & sql, const Transaction * transaction = nullptr, const std::string & branch = "main") {
            std::vector<std::string> rows;
            std::mutex mutex;
            QueryCompiler::execute(sql, *db, branch, [&](Native::Sql::SqlTuple * tuple) {
                std::lock_guard<std::mutex> lock(mutex);
                rows.push_back(Native::Sql::toString(*tuple));
            }, nullptr, transaction);
            std::sort(rows.begin(), rows.end());
            return rows;
        }

        /// \returns x of the tuple k, as text
        std::string x(int k, const Transaction * transaction = nullptr, const std::string & branch = "main") {
            auto rows = query("select x from t where k = " + std::to_string(k) + ";", transaction, branch);
            return rows.size() == 1 ? rows.front() : "(" + std::to_string(rows.size()) + " rows)";
        }

        /// Expects \p f to throw a TransactionError with \p sqlState
        static void expectError(const std::string & sqlState, const std::function<void()> & f) {
            std::string thrown = "none";
            try {
                f();
            } catch (const TransactionError & e) {
                thrown = e.sqlState;
            }
            EXPECT_EQ(thrown, sqlState);
        }

        std::unique_ptr<Database> db;
    };

    TEST_F(TransactionTest, ReadsItsSnapshotAndItsOwnWrites) {
        Transaction t = QueryCompiler::beginTransaction(*db, "main");
        exec("update t set x = 11 where k = 1;", &t);
        exec("insert into t ( k , x ) values ( 4 , 40 );", &t);
        EXPECT_EQ(x(1, &t), "(11)");
        EXPECT_EQ(x(4, &t), "(40)");
        // others do not see the writes before the commit
        EXPECT_EQ(x(1), "(10)");
        EXPECT_EQ(x(4), "(0 rows)");

        // nor does the transaction see a write that commits after its BEGIN
        exec("update t set x = 21 where k = 2;");
        EXPECT_EQ(x(2, &t), "(20)");
        EXPECT_EQ(x(2), "(21)");
        // VERSION main names the snapshot in the transaction
        EXPECT_EQ(query("select x from t version main where k = 1;", &t), (std::vector<std::string>{"(11)"}));

        QueryCompiler::commitTransaction(*db, t);
        EXPECT_EQ(query("select k , x from t;"), (std::vector<std::string>{"(1, 11)", "(2, 21)", "(3, 30)", "(4, 40)"}));
        EXPECT_EQ(db->_branches.size(), 1u); // the snapshot was dropped
    }

    TEST_F(TransactionTest, RollbackDiscardsTheWrites) {
        Transaction t = QueryCompiler::beginTransaction(*db, "main");
        exec("update t set x = 0;", &t);
        exec("delete from t where k = 2;", &t);
        exec("insert into t ( k , x ) values ( 4 , 40 );", &t);
        QueryCompiler::rollbackTransaction(*db, t);
        EXPECT_EQ(query("select k , x from t;"), (std::vector<std::string>{"(1, 10)", "(2, 20)", "(3, 30)"}));
        EXPECT_EQ(db->_branches.size(), 1u);
    }

    // two transactions that write the same tuple, even to the same value: the second commit fails, and
    // none of its writes is committed
    TEST_F(TransactionTest, FirstCommitterWins) {
        Transaction first = QueryCompiler::beginTransaction(*db, "main");
        Transaction second = QueryCompiler::beginTransaction(*db, "main");
        exec("update t set x = x + 1 where k = 1;", &first);
        exec("update t set x = x + 1 where k = 1;", &second);
        exec("update t set x = 33 where k = 3;", &second);
        QueryCompiler::commitTransaction(*db, first);
        expectError("40001", [&] { QueryCompiler::commitTransaction(*db, second); });
        EXPECT_EQ(query("select k , x from t;"), (std::vector<std::string>{"(1, 11)", "(2, 20)", "(3, 30)"}));
        EXPECT_EQ(db->_branches.size(), 1u);

        // a statement outside of transactions commits at once, and first
        Transaction third = QueryCompiler::beginTransaction(*db, "main");
        exec("delete from t where k = 2;", &third);
        exec("update t set x = 22 where k = 2;");
        expectError("40001", [&] { QueryCompiler::commitTransaction(*db, third); });
        EXPECT_EQ(x(2), "(22)");
    }

    TEST_F(TransactionTest, DisjointWritesBothCommit) {
        exec("create branch b from main;");
        Transaction first = QueryCompiler::beginTransaction(*db, "b");
        Transaction second = QueryCompiler::beginTransaction(*db, "b");
        exec("update t set x = 12 where k = 1;", &first);
        exec("update t set x = 23 where k = 2;", &second);
        exec("insert into t ( k , x ) values ( 5 , 50 );", &second);
        QueryCompiler::commitTransaction(*db, second);
        QueryCompiler::commitTransaction(*db, first);
        EXPECT_EQ(query("select k , x from t;", nullptr, "b"), (std::vector<std::string>{"(1, 12)", "(2, 23)", "(3, 30)", "(5, 50)"}));
        EXPECT_EQ(query("select k , x from t;"), (std::vector<std::string>{"(1, 10)", "(2, 20)", "(3, 30)"}));
    }

    TEST_F(TransactionTest, ConfinesItsStatements) {
        exec("create branch b from main;");
        Transaction t = QueryCompiler::beginTransaction(*db, "main");
        // a rollback could not undo a change of the catalog, and a commit merges the snapshot only
        expectError("25001", [&] { exec("create branch c from main;", &t); });
        expectError("25001", [&] { exec("alter table t add column y integer default 0;", &t); });
        expectError("25001", [&] { exec("update t version b set x = 0;", &t); });
        EXPECT_EQ(x(1, &t, "b"), "(10)"); // other branches may be read
        EXPECT_EQ(query("select x from t version b where k = 1;", &t), (std::vector<std::string>{"(10)"}));

        // the snapshot is invisible to the other statements: no name reaches it, VERSION ALL and LEAVES
        // leave it out, and its branch cannot be dropped while it exists
        exec("update t set x = 0 where k = 1;", &t);
        EXPECT_ANY_THROW(exec("select x from t version " + t.snapshot + ";"));
        EXPECT_EQ(query("select branch , sum(x) from t version all group by branch;"),
                  (std::vector<std::string>{"(b, 60)", "(master, 60)"}));
        EXPECT_EQ(query("select branch , sum(x) from t version all leaves of main group by branch;"),
                  (std::vector<std::string>{"(b, 60)"}));
        Transaction inB = QueryCompiler::beginTransaction(*db, "b");
        EXPECT_ANY_THROW(exec("drop branch b;"));
        QueryCompiler::rollbackTransaction(*db, inB);
        exec("drop branch b;");
        QueryCompiler::commitTransaction(*db, t);
        EXPECT_EQ(x(1), "(0)");
    }

    // a dropped snapshot folds the identifier main continued under into the one it forked from, and both
    // new identifiers are reused: their number stays bounded by the forks that are alive
    TEST_F(TransactionTest, IdentifiersStayBounded) {
        const size_t before = db->getIdentCount();
        for (int i = 0; i < 200; ++i) {
            Transaction t = QueryCompiler::beginTransaction(*db, "main");
            exec("update t set x = x + 1 where k = 1;", &t);
            if (i % 2 == 0) QueryCompiler::commitTransaction(*db, t);
            else QueryCompiler::rollbackTransaction(*db, t);
        }
        EXPECT_EQ(x(1), "(110)");
        EXPECT_LE(db->getIdentCount(), before + 3); // two of a fork, and the one of dropped inserts
        EXPECT_EQ(db->getIdent(master_branch_id), 0u);
    }

    // the tuples a rolled-back transaction or a dropped branch inserted stay invisible once their
    // identifiers are given to new branches
    TEST_F(TransactionTest, ReusedIdentifiersHideDroppedInserts) {
        Transaction t = QueryCompiler::beginTransaction(*db, "main");
        exec("insert into t ( k , x ) values ( 7 , 70 );", &t);
        QueryCompiler::rollbackTransaction(*db, t);
        exec("create branch b from main;");
        exec("insert into t version b ( k , x ) values ( 8 , 80 );");
        exec("drop branch b;");
        Transaction u = QueryCompiler::beginTransaction(*db, "main");
        exec("create branch c from main;");
        exec("update t version c set x = 31 where k = 3;");
        const std::vector<std::string> rows = {"(1, 10)", "(2, 20)", "(3, 30)"};
        EXPECT_EQ(query("select k , x from t;", &u), rows);
        EXPECT_EQ(query("select k , x from t;"), rows);
        EXPECT_EQ(query("select k , x from t;", nullptr, "c"), (std::vector<std::string>{"(1, 10)", "(2, 20)", "(3, 31)"}));
        QueryCompiler::commitTransaction(*db, u);
        exec("merge branch c into main;");
        exec("drop branch c;");
        EXPECT_EQ(query("select k , x from t;"), (std::vector<std::string>{"(1, 10)", "(2, 20)", "(3, 31)"}));
    }

    // increments that read the counter in one statement and write it in the next, from several threads
    // at once: every committed increment counts, and the conflicting ones retry
    TEST_F(TransactionTest, NoLostUpdates) {
        constexpr int workers = 6, increments = 20;
        const size_t threads = Parallel::threads;
        Parallel::threads = 2;
        std::atomic<int> conflicts(0);
        std::vector<std::string> failures;
        std::mutex failuresLatch;
        std::vector<std::thread> pool;
        for (int w = 0; w < workers; ++w) {
            pool.emplace_back([&]() {
                try {
                    for (int i = 0; i < increments; ) {
                        Transaction t = QueryCompiler::beginTransaction(*db, "main");
                        std::string value = x(1, &t);
                        int64_t v = std::stoll(value.substr(1, value.size() - 2));
                        exec("update t set x = " + std::to_string(v + 1) + " where k = 1;", &t);
                        try {
                            QueryCompiler::commitTransaction(*db, t);
                            ++i;
                        } catch (const TransactionError & e) {
                            if (e.sqlState != "40001") throw;
                            ++conflicts;
                        }
                    }
                } catch (const std::exception & e) {
                    std::lock_guard<std::mutex> lock(failuresLatch);
                    failures.push_back(e.what());
                }
            });
        }
        for (auto & thread : pool) thread.join();
        Parallel::threads = threads;
        EXPECT_TRUE(failures.empty()) << (failures.empty() ? "" : failures.front());
        EXPECT_EQ(x(1), "(" + std::to_string(10 + workers * increments) + ")");
        EXPECT_EQ(db->_branches.size(), 1u);
        std::cout << "[          ] " << conflicts << " conflicting commits retried\n";
    }

}
