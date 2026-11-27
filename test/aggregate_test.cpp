// Aggregation: sum and avg of arrays (element by element), aggregates of expressions and in
// expressions, GROUP BY columns, aggregates over joins and common table expressions, min and max of any
// type, groups of no rows, HAVING, the errors of PostgreSQL, and mini-batch gradient descent with avg

#include "codegen/LLVMCompat.hpp"

#include <algorithm>
#include <mutex>
#include <regex>

#include "foundations/Parallel.hpp"
#include "runtime/SqlTuple.hpp"
#include "queryCompiler/queryCompiler.hpp"
#include <gtest/gtest.h>

namespace {

    class AggregateTest : public ::testing::Test {
    protected:
        void SetUp() override {
            db = std::make_unique<Database>();
            exec("CREATE TABLE m ( g integer not null, name varchar(5), a double precision[], x integer, d date );");
            exec("INSERT INTO m VALUES (1, 'b', '{1,2}', 10, '2025-01-02'), (1, 'a', '{3,4}', 20, '2025-01-01'), "
                 "(2, NULL, '{5,6}', NULL, NULL), (2, 'c', NULL, 7, '2025-03-01'), (3, 'd', '{NULL,1}', 1, '2025-02-01');");
        }

        QueryCompiler::StatementResult exec(const std::string & sql) {
            return QueryCompiler::execute(sql, *db, "master", [](Native::Sql::SqlTuple *) { });
        }

        /// \returns The rows of the query, sorted, the same with one thread and with parallel scans
        std::vector<std::string> query(const std::string & sql) {
            auto run = [&]() {
                std::vector<std::string> rows;
                std::mutex mutex;
                QueryCompiler::execute(sql, *db, "master", [&](Native::Sql::SqlTuple * tuple) {
                    std::lock_guard<std::mutex> lock(mutex);
                    rows.push_back(Native::Sql::toString(*tuple));
                });
                std::sort(rows.begin(), rows.end());
                return rows;
            };
            const size_t threads = Parallel::threads, morselSize = Parallel::morselSize;
            auto rows = run();
            Parallel::threads = 4;
            Parallel::morselSize = 2;
            auto parallel = run();
            Parallel::threads = threads;
            Parallel::morselSize = morselSize;
            EXPECT_EQ(rows, parallel) << sql;
            return rows;
        }

        using rows_t = std::vector<std::string>;
        std::unique_ptr<Database> db;
    };

    TEST_F(AggregateTest, SumAndAvgOfArrays) {
        // element by element; NULL arrays do not count, a NULL element makes the element NULL
        EXPECT_EQ(query("SELECT sum(a), avg(a), count(a) FROM m WHERE g < 3;"), (rows_t{"({9,12}, {3,4}, 3)"}));
        EXPECT_EQ(query("SELECT sum(a) FROM m;"), (rows_t{"({NULL,13})"}));
        auto described = exec("SELECT sum(a) AS s, avg(a) FROM m;");
        EXPECT_EQ(described.columnNames, (std::vector<std::string>{"s", "avg"}));
        EXPECT_EQ(described.columnTypes[0].typeID, Sql::SqlType::TypeID::ArrayID);
        // of expressions over arrays, and in expressions
        EXPECT_EQ(query("SELECT sum(a ** '{{1},{1}}'), 2 * avg(a) - 1 FROM m WHERE g = 1;"), (rows_t{"({10}, {3,5})"}));
        exec("INSERT INTO m VALUES (4, 'e', '{1,2,3}', 0, NULL);");
        EXPECT_THROW(exec("SELECT sum(a) FROM m;"), std::exception); // arrays of different shapes
        EXPECT_THROW(exec("SELECT avg(name) FROM m;"), std::exception);
    }

    TEST_F(AggregateTest, GroupByColumns) {
        EXPECT_EQ(query("SELECT g, count(*), count(x), sum(x), avg(x), min(x), max(x) FROM m GROUP BY g;"),
                  (rows_t{"(1, 2, 2, 30, 15, 10, 20)", "(2, 2, 1, 7, 7, 7, 7)", "(3, 1, 1, 1, 1, 1, 1)"}));
        // a column of the select list that GROUP BY names, NULL as a group of its own
        EXPECT_EQ(query("SELECT name, count(*) FROM m GROUP BY name;"),
                  (rows_t{"(NULL, 1)", "(a, 1)", "(b, 1)", "(c, 1)", "(d, 1)"}));
        EXPECT_EQ(query("SELECT g, sum(a) FROM m GROUP BY g, m.g;"), (rows_t{"(1, {4,6})", "(2, {5,6})", "(3, {NULL,1})"}));
        // min and max of text, dates and arrays
        EXPECT_EQ(query("SELECT min(name), max(name), min(d), max(d), min(a), max(a) FROM m;"),
                  (rows_t{"(a, d, 2025-01-01, 2025-03-01, {1,2}, {NULL,1})"}));
        // aggregates of expressions and in expressions, over groups
        EXPECT_EQ(query("SELECT g * 10 + 1, max(x) - min(x), sum(x * 2) FROM m WHERE x IS NOT NULL GROUP BY g;"),
                  (rows_t{"(11, 10, 60)", "(21, 0, 14)", "(31, 0, 2)"}));
    }

    TEST_F(AggregateTest, JoinsAndCommonTableExpressions) {
        exec("CREATE TABLE w ( g integer not null, f double precision[] );");
        exec("INSERT INTO w VALUES (1, '{10,100}'), (2, '{1,1}');");
        EXPECT_EQ(query("SELECT w.g, sum(m.a * w.f) FROM m, w WHERE m.g = w.g GROUP BY w.g;"), (rows_t{"(1, {40,600})", "(2, {5,6})"}));
        EXPECT_EQ(query("WITH c AS (SELECT g, a FROM m WHERE a IS NOT NULL) SELECT count(*), avg(a) FROM c WHERE g < 3;"),
                  (rows_t{"(3, {3,4})"}));
        // aggregates in a common table expression, and over one
        EXPECT_EQ(query("WITH s AS (SELECT g, sum(x) AS total FROM m GROUP BY g) SELECT max(total), min(total) FROM s;"), (rows_t{"(30, 1)"}));
    }

    TEST_F(AggregateTest, NoRowsAndTypes) {
        // without GROUP BY one row, also of no rows; with GROUP BY none
        EXPECT_EQ(query("SELECT count(*), sum(a), avg(x * 2) FROM m WHERE g > 9;"), (rows_t{"(0, NULL, NULL)"}));
        EXPECT_TRUE(query("SELECT g, count(*) FROM m WHERE g > 9 GROUP BY g;").empty());
        // the types of the aggregation over branches: sum(x) there and sum(x + 0) here agree
        auto branchPath = exec("SELECT sum(x), avg(x) FROM m;"), general = exec("SELECT sum(x + 0), avg(x + 0) FROM m;");
        EXPECT_EQ(branchPath.columnTypes[0].typeID, general.columnTypes[0].typeID);
        EXPECT_EQ(branchPath.columnTypes[1].typeID, general.columnTypes[1].typeID);
        EXPECT_EQ(query("SELECT sum(x), avg(x) FROM m;"), query("SELECT sum(x + 0), avg(x + 0) FROM m;"));
    }

    TEST_F(AggregateTest, Errors) {
        EXPECT_THROW(exec("SELECT g, x, count(*) FROM m GROUP BY g;"), std::exception);  // x neither grouped nor aggregated
        EXPECT_THROW(exec("SELECT sum(max(x)) FROM m;"), std::exception);                // nested
        EXPECT_THROW(exec("SELECT * FROM m GROUP BY g;"), std::exception);
        EXPECT_THROW(exec("SELECT sum(x, g) FROM m;"), std::exception);
        EXPECT_EQ(query("SELECT count(*) FROM m;"), (rows_t{"(5)"})); // the database still answers
    }

    TEST_F(AggregateTest, Having) {
        // of aggregates of the select list, of others, and of grouped columns
        EXPECT_EQ(query("SELECT g, count(*) FROM m GROUP BY g HAVING count(*) > 1;"), (rows_t{"(1, 2)", "(2, 2)"}));
        EXPECT_EQ(query("SELECT g FROM m GROUP BY g HAVING sum(x) > 5;"), (rows_t{"(1)", "(2)"}));
        EXPECT_EQ(query("SELECT g, max(x) FROM m GROUP BY g having g >= 2 AND max(x) < 10;"), (rows_t{"(2, 7)", "(3, 1)"}));
        // after WHERE; a NULL condition keeps no group
        EXPECT_EQ(query("SELECT g, count(*) FROM m WHERE x IS NOT NULL GROUP BY g HAVING count(*) = 1;"), (rows_t{"(2, 1)", "(3, 1)"}));
        EXPECT_EQ(query("SELECT name FROM m GROUP BY name HAVING max(x) > 5;"), (rows_t{"(a)", "(b)", "(c)"}));
        // without GROUP BY, all rows are one group
        EXPECT_EQ(query("SELECT count(*) FROM m HAVING count(*) > 3;"), (rows_t{"(5)"}));
        EXPECT_TRUE(query("SELECT count(*) FROM m HAVING count(*) > 9;").empty());
        EXPECT_EQ(query("SELECT sum(x) FROM m t HAVING min(t.x) = 1;"), (rows_t{"(38)"}));
        // of arrays
        EXPECT_EQ(query("SELECT g FROM m GROUP BY g HAVING cardinality(sum(a)) = 2 AND count(a) = 2;"), (rows_t{"(1)"}));
        // in common table expressions, also in the step of a recursive one
        EXPECT_EQ(query("WITH s AS (SELECT g, sum(x) AS total FROM m GROUP BY g HAVING sum(x) > 5) SELECT count(*) FROM s;"), (rows_t{"(2)"}));
        EXPECT_EQ(query("WITH RECURSIVE r(i) AS (SELECT 1 UNION ALL SELECT r.i + 1 FROM r, m WHERE r.i < 3 GROUP BY r.i "
                        "HAVING count(*) = 5) SELECT max(i) FROM r;"), (rows_t{"(3)"}));
        // errors
        EXPECT_THROW(exec("SELECT g FROM m GROUP BY g HAVING x > 1;"), std::exception);  // x neither grouped nor aggregated
        EXPECT_THROW(exec("SELECT g FROM m GROUP BY g HAVING;"), std::exception);
        EXPECT_THROW(exec("SELECT g FROM m GROUP BY g HAVING count(*) > 1 GROUP BY g;"), std::exception);
        EXPECT_THROW(exec("SELECT count(*) FROM m VERSION ALL HAVING count(*) > 1;"), std::exception);
        EXPECT_THROW(exec("SELECT branch, count(*) FROM m GROUP BY branch HAVING count(*) > 1;"), std::exception);
        EXPECT_EQ(query("SELECT count(*) FROM m;"), (rows_t{"(5)"}));
    }

    TEST_F(AggregateTest, MiniBatchGradientDescent) {
        // least squares of y = 1 + 2 x, one sample per row: w - 0.2 * avg(x^T (x w - y)) per step
        exec("CREATE TABLE samples ( x double precision[], y double precision[] );");
        exec("INSERT INTO samples VALUES ('{{1,0}}', '{{1}}'), ('{{1,1}}', '{{3}}'), ('{{1,2}}', '{{5}}'), ('{{1,3}}', '{{7}}');");
        auto rows = query("WITH RECURSIVE gd(i, w) AS (SELECT 0, '{{0},{0}}'::double precision[] "
                          "UNION ALL SELECT gd.i + 1, gd.w - 0.2 * avg(transpose(s.x) ** ((s.x ** gd.w) - s.y)) "
                          "FROM gd, samples s WHERE gd.i < 300 GROUP BY gd.i, gd.w) SELECT w FROM gd WHERE i = 300;");
        ASSERT_EQ(rows.size(), 1u);
        std::regex number(R"([-0-9.e+]+)");
        std::vector<double> w;
        for (std::sregex_iterator it(rows[0].begin(), rows[0].end(), number), end; it != end; ++it) w.push_back(std::stod(it->str()));
        ASSERT_EQ(w.size(), 2u) << rows[0];
        EXPECT_NEAR(w[0], 1.0, 1e-5);
        EXPECT_NEAR(w[1], 2.0, 1e-5);
    }

}
