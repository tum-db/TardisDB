// Queries in one plan: GROUP BY in a hash aggregation, recursive queries in a loop of the generated code (Recursive Union
// over the working table), subqueries in FROM as parts of the plan, and the build side of hash joins

#include "codegen/LLVMCompat.hpp"
#include <algorithm>
#include <mutex>
#include "foundations/Parallel.hpp"
#include "native/sql/SqlTuple.hpp"
#include "queryCompiler/queryCompiler.hpp"
#include <gtest/gtest.h>

namespace {

    using rows_t = std::vector<std::string>;

    class CompiledQueryTest : public ::testing::Test {
    protected:
        void SetUp() override {
            db = std::make_unique<Database>();
            exec("CREATE TABLE cg ( k integer, s text, x double precision, n numeric(10,2), b bigint, a double precision[] );");
            exec("INSERT INTO cg VALUES (1, 'a', 1.5, 1.25, 10, '{1,2}'), (1, 'b', 2.5, 2.50, 20, '{3,4}'), (2, 'a', NULL, NULL, NULL, NULL), "
                 "(NULL, 'c', 4, 3.00, 40, '{5,6}'), (NULL, 'd', 5, 1.00, 50, '{7,8}');");
        }

        QueryCompiler::StatementResult exec(const std::string & sql) {
            return QueryCompiler::execute(sql, *db, "master", [](Native::Sql::SqlTuple *) { });
        }

        rows_t run(const std::string & sql) {
            rows_t rows;
            std::mutex mutex;
            QueryCompiler::execute(sql, *db, "master", [&](Native::Sql::SqlTuple * tuple) {
                std::lock_guard<std::mutex> lock(mutex);
                rows.push_back(Native::Sql::toString(*tuple));
            });
            return rows;
        }

        std::string plan(const std::string & sql) {
            std::string text;
            QueryCompiler::execute("EXPLAIN " + sql, *db, "master", [&](Native::Sql::SqlTuple * tuple) { text = Native::Sql::toString(*tuple); });
            return text;
        }

        static bool has(const std::string & text, const std::string & part) { return text.find(part) != std::string::npos; }

        std::unique_ptr<Database> db;
    };

    TEST_F(CompiledQueryTest, Aggregation) {
        // NULL keys are one group, NULLs are skipped, the types of the aggregates as before
        EXPECT_EQ(run("SELECT k, count(*), count(x), sum(x), avg(x), min(x), max(s), sum(n), avg(n), sum(b), avg(k), min(b), sum(a), avg(a) "
                      "FROM cg GROUP BY k ORDER BY k;"),
                  (rows_t{"(1, 2, 2, 4, 2, 1.5, b, 3.75, 1.87, 30, 1, 10, {4,6}, {2,3})",
                          "(2, 1, 0, NULL, NULL, NULL, a, NULL, NULL, NULL, 2, NULL, NULL, NULL)",
                          "(NULL, 2, 2, 9, 4.5, 4, d, 4.00, 2.00, 90, NULL, 40, {12,14}, {6,7})"}));
        // aggregates of expressions, in expressions, HAVING, over a join, several keys
        EXPECT_EQ(run("SELECT count(*), sum(x * 2), max(k) FROM cg;"), (rows_t{"(5, 26, 2)"}));
        EXPECT_EQ(run("SELECT k, sum(x) + 1 AS t FROM cg GROUP BY k HAVING count(*) > 1 ORDER BY k;"), (rows_t{"(1, 5)", "(NULL, 10)"}));
        EXPECT_EQ(run("SELECT c.s, count(*) FROM cg c, cg d WHERE c.k = d.k GROUP BY c.s ORDER BY c.s;"), (rows_t{"(a, 3)", "(b, 2)"}));
        EXPECT_EQ(run("SELECT k, s, count(*) FROM cg GROUP BY k, s ORDER BY s, k;"), (rows_t{"(1, a, 1)", "(2, a, 1)", "(1, b, 1)", "(NULL, c, 1)", "(NULL, d, 1)"}));
        const std::string p = plan("SELECT k, sum(x * 2) FROM cg GROUP BY k;");
        EXPECT_TRUE(has(p, "\"Node Type\": \"HashAggregate\"") && !has(p, "\"Queries\"")) << p;
        // over no rows: one group without GROUP BY, none with it
        EXPECT_EQ(run("SELECT count(*), sum(c.x), max(c.s) FROM cg c, cg d WHERE c.k > 100;"), (rows_t{"(0, NULL, NULL)"}));
        EXPECT_EQ(run("SELECT k, count(*) FROM cg WHERE k > 100 GROUP BY k;"), rows_t{});
        EXPECT_EQ(run("SELECT count(*), sum(x), min(s) FROM cg WHERE k > 100;"), (rows_t{"(0, NULL, NULL)"}));
    }

    TEST_F(CompiledQueryTest, ParallelAggregation) {
        // the workers of a parallel scan aggregate into groups of their own, merged after the scan: the rows of one worker
        exec("CREATE TABLE pa ( g integer not null, v double precision, s text, a double precision[] );");
        std::string values;
        for (int i = 0; i < 200; ++i) {
            values += std::string(i ? ", " : "") + "(" + std::to_string(i % 7) + ", " + (i % 11 ? std::to_string(i) + ".5" : "NULL") +
                      ", 's" + std::to_string(i % 13) + "', '{" + std::to_string(i) + ",1}')";
        }
        exec("INSERT INTO pa VALUES " + values + ";");
        const std::vector<std::string> queries = {
            "SELECT count(*), count(v), sum(v), min(v), max(s), avg(g), sum(a) FROM pa WHERE g > 0;",
            "SELECT g, count(*), sum(v), max(v), min(s), avg(a) FROM pa GROUP BY g ORDER BY g;",
            "SELECT s, g, count(*) FROM pa WHERE v > 20 GROUP BY s, g ORDER BY s, g;"};
        const size_t threads = Parallel::threads, morselSize = Parallel::morselSize;
        std::vector<rows_t> serial;
        Parallel::threads = 1;
        for (auto & q : queries) serial.push_back(run(q));
        Parallel::threads = 4;
        Parallel::morselSize = 3;
        for (size_t i = 0; i < queries.size(); ++i) EXPECT_EQ(run(queries[i]), serial[i]) << queries[i];
        Parallel::threads = threads;
        Parallel::morselSize = morselSize;
        EXPECT_EQ(serial[0].size(), 1u);
        EXPECT_EQ(serial[1].size(), 7u);
    }

    TEST_F(CompiledQueryTest, Recursion) {
        // one plan: the loop runs the steps over the working table
        const std::string p = plan("ANALYZE WITH RECURSIVE r (i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM r WHERE i < 3) SELECT i FROM r;");
        EXPECT_TRUE(has(p, "\"Node Type\": \"Recursive Union\"") && has(p, "\"Node Type\": \"WorkTable Scan\"")) << p;
        EXPECT_EQ(run("WITH RECURSIVE r (i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM r WHERE i < 100000) SELECT count(*), sum(i) FROM r;"),
                  (rows_t{"(100000, 5000050000)"}));
        // NULLs, doubles, arrays, a join with a table and aggregates in the step
        EXPECT_EQ(run("WITH RECURSIVE r (i, v, a) AS (SELECT 1, NULL::float, '{1,1}'::float[] UNION ALL "
                      "SELECT i + 1, 2.0 * i, a * 2 FROM r WHERE i < 3) SELECT * FROM r ORDER BY i;"),
                  (rows_t{"(1, NULL, {1,1})", "(2, 2, {2,2})", "(3, 4, {4,4})"}));
        EXPECT_EQ(run("WITH RECURSIVE r (i, t) AS (SELECT 0, 0::float UNION ALL SELECT i + 1, t + sum(x) FROM r, cg WHERE i < 2 GROUP BY i, t) "
                      "SELECT * FROM r ORDER BY i;"), (rows_t{"(0, 0)", "(1, 13)", "(2, 26)"}));
        // subqueries in FROM of the step, which read the working table
        EXPECT_EQ(run("WITH RECURSIVE r (i, w) AS (SELECT 0, '{{1,2},{3,4}}'::float[] UNION ALL SELECT i + 1, w - 0.5 * sum(transpose(a2) ** d) FROM ("
                      "SELECT a2 * 1 AS d, * FROM (SELECT array[a] AS a2, * FROM (SELECT * FROM cg WHERE a IS NOT NULL), r WHERE i < 1)) GROUP BY i, w) "
                      "SELECT i, w FROM r ORDER BY i;"), (rows_t{"(0, {{1,2},{3,4}})", "(1, {{-41,-48},{-47,-56}})"}));
        // UNION without ALL and strings run as queries per step
        EXPECT_EQ(run("WITH RECURSIVE r (i) AS (SELECT 1 UNION SELECT i + 1 FROM r WHERE i < 3) SELECT i FROM r ORDER BY i;"), (rows_t{"(1)", "(2)", "(3)"}));
        EXPECT_EQ(run("WITH RECURSIVE r (s) AS (SELECT CAST('a' AS varchar(5)) UNION ALL SELECT CAST('b' AS varchar(5)) FROM r WHERE s = 'a') "
                      "SELECT s FROM r ORDER BY s;"), (rows_t{"(a)", "(b)"}));
    }

    TEST_F(CompiledQueryTest, Subqueries) {
        // parts of the plan, also with aggregates
        const std::string p = plan("ANALYZE SELECT x FROM (SELECT x * 2 AS x FROM cg WHERE k = 1) s;");
        EXPECT_TRUE(!has(p, "\"Queries\"") && has(p, "\"Relation Name\": \"cg\"")) << p;
        EXPECT_EQ(run("SELECT sum(t) FROM (SELECT k, sum(x) AS t FROM cg GROUP BY k) s;"), (rows_t{"(13)"}));
        EXPECT_EQ(run("SELECT m FROM (SELECT max(CAST(s AS varchar(5))) AS m FROM cg) v;"), (rows_t{"(d)"}));
    }

    TEST_F(CompiledQueryTest, HashJoinBuildSide) {
        // the hash table of the smaller table, whichever comes first
        exec("CREATE TABLE one ( k integer );");
        exec("INSERT INTO one VALUES (1);");
        for (const std::string & from : {"one, cg", "cg, one"}) {
            const std::string p = plan("SELECT count(*) FROM " + from + " WHERE one.k = cg.k;");
            EXPECT_LT(p.find("\"Relation Name\": \"one\""), p.find("\"Relation Name\": \"cg\"")) << p;
            EXPECT_EQ(run("SELECT count(*) FROM " + from + " WHERE one.k = cg.k;"), (rows_t{"(2)"}));
        }
    }

} // namespace
