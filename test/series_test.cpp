// generate_series(start, stop [, step]): in FROM (with aliases), in the select list of a query without
// FROM, of integers, bigints and numerics, empty and descending series, in joins, aggregates, ORDER BY,
// WITH and set operations, and the errors

#include "codegen/LLVMCompat.hpp"
#include <algorithm>
#include <mutex>
#include "foundations/Parallel.hpp"
#include "runtime/SqlTuple.hpp"
#include "queryCompiler/queryCompiler.hpp"
#include <gtest/gtest.h>

namespace {

    using rows_t = std::vector<std::string>;

    class SeriesTest : public ::testing::Test {
    protected:
        void SetUp() override {
            db = std::make_unique<Database>();
            exec("CREATE TABLE st ( id integer not null, name varchar(10) not null );");
            exec("INSERT INTO st VALUES (1, 'one'), (2, 'two'), (3, 'three');");
        }

        QueryCompiler::StatementResult exec(const std::string & sql) {
            return QueryCompiler::execute(sql, *db, "master", [](Native::Sql::SqlTuple *) { });
        }

        /// \returns The rows in the order of the result
        rows_t run(const std::string & sql, std::vector<std::string> * names = nullptr) {
            rows_t rows;
            std::mutex mutex;
            QueryCompiler::execute(sql, *db, "master", [&](Native::Sql::SqlTuple * tuple) {
                std::lock_guard<std::mutex> lock(mutex);
                rows.push_back(Native::Sql::toString(*tuple));
            }, [&](const QueryCompiler::StatementResult & result) { if (names) *names = result.columnNames; });
            return rows;
        }

        rows_t sorted(const std::string & sql) {
            rows_t rows = run(sql);
            std::sort(rows.begin(), rows.end());
            return rows;
        }

        std::unique_ptr<Database> db;
    };

    TEST_F(SeriesTest, From) {
        std::vector<std::string> names;
        EXPECT_EQ(run("SELECT * FROM generate_series(1, 5) ORDER BY 1;", &names), (rows_t{"(1)", "(2)", "(3)", "(4)", "(5)"}));
        EXPECT_EQ(names, std::vector<std::string>{"generate_series"});
        EXPECT_EQ(run("SELECT * FROM generate_series(1, 10, 3) g ORDER BY g;", &names), (rows_t{"(1)", "(4)", "(7)", "(10)"}));
        EXPECT_EQ(names, std::vector<std::string>{"g"});
        EXPECT_EQ(run("SELECT g FROM generate_series(5, 1, -2) AS g ORDER BY g DESC;"), (rows_t{"(5)", "(3)", "(1)"}));
        EXPECT_EQ(run("SELECT x.n FROM generate_series(1, 3) AS x(n) ORDER BY x.n;", &names), (rows_t{"(1)", "(2)", "(3)"}));
        EXPECT_EQ(names, std::vector<std::string>{"n"});
        // constant expressions, NULL and empty series
        EXPECT_EQ(sorted("SELECT * FROM generate_series(1, 3 + 2, 1 * 2);"), (rows_t{"(1)", "(3)", "(5)"}));
        EXPECT_EQ(run("SELECT * FROM generate_series(1, NULL);"), rows_t{});
        EXPECT_EQ(run("SELECT * FROM generate_series(5, 1);"), rows_t{});
        EXPECT_EQ(run("SELECT * FROM generate_series(1, 5, -1);"), rows_t{});
        EXPECT_EQ(run("SELECT * FROM generate_series(7, 7);"), (rows_t{"(7)"}));
    }

    TEST_F(SeriesTest, Types) {
        // bigint, numeric (the largest scale), the limits of integer
        EXPECT_EQ(sorted("SELECT * FROM generate_series(3000000000, 3000000002);"), (rows_t{"(3000000000)", "(3000000001)", "(3000000002)"}));
        EXPECT_EQ(sorted("SELECT * FROM generate_series(0, 1, 0.25);"), (rows_t{"(0.00)", "(0.25)", "(0.50)", "(0.75)", "(1.00)"}));
        EXPECT_EQ(sorted("SELECT * FROM generate_series(1.5, 3);"), (rows_t{"(1.5)", "(2.5)"}));
        // a quoted number (a parameter of unknown type) is a number
        EXPECT_EQ(sorted("SELECT * FROM generate_series('1', 3);"), (rows_t{"(1)", "(2)", "(3)"}));
        EXPECT_EQ(sorted("SELECT * FROM generate_series('0.5', 1);"), (rows_t{"(0.5)"}));
        EXPECT_EQ(sorted("SELECT * FROM generate_series(-2147483648, -2147483647);"), (rows_t{"(-2147483647)", "(-2147483648)"}));
        EXPECT_EQ(sorted("SELECT * FROM generate_series(2147483646, 2147483647);"), (rows_t{"(2147483646)", "(2147483647)"}));
    }

    TEST_F(SeriesTest, SelectList) {
        std::vector<std::string> names;
        EXPECT_EQ(sorted("SELECT generate_series(1, 3);"), (rows_t{"(1)", "(2)", "(3)"}));
        EXPECT_EQ(run("SELECT generate_series(1, 3) AS i ORDER BY i DESC;", &names), (rows_t{"(3)", "(2)", "(1)"}));
        EXPECT_EQ(names, std::vector<std::string>{"i"});
        EXPECT_EQ(sorted("SELECT generate_series(1, 3) * 10;"), (rows_t{"(10)", "(20)", "(30)"}));
    }

    TEST_F(SeriesTest, Queries) {
        EXPECT_EQ(sorted("SELECT st.name, g FROM st, generate_series(1, 2) g WHERE st.id = g;"), (rows_t{"(one, 1)", "(two, 2)"}));
        EXPECT_EQ(sorted("SELECT st.name, s.k FROM generate_series(2, 9) s(k), st WHERE s.k = st.id + 1;"),
                  (rows_t{"(one, 2)", "(three, 4)", "(two, 3)"}));
        EXPECT_EQ(run("SELECT count(*), sum(g) FROM generate_series(1, 100) g;"), (rows_t{"(100, 5050)"}));
        EXPECT_EQ(run("SELECT count(*) FROM generate_series(1, 10) a, generate_series(1, 10) b WHERE a = b;"), (rows_t{"(10)"}));
        EXPECT_EQ(run("SELECT g FROM generate_series(1, 10) g WHERE g > 7 ORDER BY g DESC;"), (rows_t{"(10)", "(9)", "(8)"}));
        EXPECT_EQ(sorted("WITH s AS (SELECT * FROM generate_series(1, 3)) SELECT * FROM s;"), (rows_t{"(1)", "(2)", "(3)"}));
        EXPECT_EQ(sorted("SELECT * FROM generate_series(1, 2) UNION ALL SELECT * FROM generate_series(10, 11);"),
                  (rows_t{"(1)", "(10)", "(11)", "(2)"}));
        // the plan reads a table
        std::string plan;
        QueryCompiler::execute("EXPLAIN SELECT g FROM generate_series(1, 3) g;", *db, "master",
                               [&](Native::Sql::SqlTuple * tuple) { plan = Native::Sql::toString(*tuple); });
        EXPECT_NE(plan.find("Table Scan"), std::string::npos) << plan;
    }

    TEST_F(SeriesTest, Errors) {
        EXPECT_THROW(exec("SELECT * FROM generate_series(1, 5, 0);"), std::exception);
        EXPECT_THROW(exec("SELECT * FROM generate_series(1);"), std::exception);
        EXPECT_THROW(exec("SELECT * FROM generate_series(1, 2, 3, 4);"), std::exception);
        EXPECT_THROW(exec("SELECT * FROM generate_series('a', 'b');"), std::exception);
        EXPECT_THROW(exec("SELECT * FROM st, generate_series(1, st.id);"), std::exception);
        EXPECT_THROW(exec("SELECT generate_series(1, 2) FROM st;"), std::exception);
        EXPECT_THROW(exec("SELECT generate_series(1, 2), generate_series(1, 3);"), std::exception);
        EXPECT_THROW(exec("SELECT * FROM generate_series(1, 3) g(a, b);"), std::exception);
    }

} // namespace
