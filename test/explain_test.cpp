// EXPLAIN [ANALYZE]: the plan as JSON (operators, conditions, row counts also of parallel scans, several
// queries), which statements it takes and that it executes them only with ANALYZE; the times of a statement

#include "codegen/LLVMCompat.hpp"

#include <regex>

#include "foundations/Parallel.hpp"
#include "runtime/SqlTuple.hpp"
#include "queryCompiler/queryCompiler.hpp"
#include <gtest/gtest.h>

namespace {

    class ExplainTest : public ::testing::Test {
    protected:
        void SetUp() override {
            db = std::make_unique<Database>();
            exec("CREATE TABLE t ( g integer not null, x integer, name text );");
            exec("CREATE TABLE u ( g integer not null, y integer );");
            exec("INSERT INTO t VALUES (1, 10, 'a'), (1, 20, 'b'), (2, 7, 'c'), (3, 1, 'd');");
            exec("INSERT INTO u VALUES (1, 100), (2, 200);");
        }

        QueryCompiler::StatementResult exec(const std::string & sql) {
            return QueryCompiler::execute(sql, *db, "master", [](Native::Sql::SqlTuple *) { });
        }

        std::vector<std::string> rows(const std::string & sql) {
            std::vector<std::string> out;
            QueryCompiler::execute(sql, *db, "master", [&](Native::Sql::SqlTuple * tuple) { out.push_back(Native::Sql::toString(*tuple)); });
            std::sort(out.begin(), out.end());
            return out;
        }

        /// \returns The plan of EXPLAIN
        std::string plan(const std::string & sql) {
            std::vector<std::string> plans;
            auto result = QueryCompiler::execute(sql, *db, "master", [&](Native::Sql::SqlTuple * tuple) {
                plans.push_back(Native::Sql::toString(*tuple->values[0]));
            });
            EXPECT_EQ(result.tag, "EXPLAIN");
            EXPECT_EQ(result.columnNames, (std::vector<std::string>{"QUERY PLAN"}));
            EXPECT_EQ(plans.size(), 1u);
            return plans.empty() ? std::string() : plans.front();
        }

        static bool has(const std::string & plan, const std::string & text) { return plan.find(text) != std::string::npos; }

        /// \returns The "Actual Rows" of the nodes, in the order of the plan
        static std::vector<int> actualRows(const std::string & plan) {
            std::vector<int> out;
            std::regex rows(R"("Actual Rows": (\d+))");
            for (std::sregex_iterator it(plan.begin(), plan.end(), rows), end; it != end; ++it) out.push_back(std::stoi((*it)[1]));
            return out;
        }

        std::unique_ptr<Database> db;
    };

    TEST_F(ExplainTest, PlanWithoutExecution) {
        const std::string p = plan("EXPLAIN SELECT g, x * 2 FROM t WHERE x > 5;");
        EXPECT_TRUE(has(p, "\"Node Type\": \"Result\"")) << p;
        EXPECT_TRUE(has(p, "\"Output\": [\"(x * 2)\"]")) << p;
        EXPECT_TRUE(has(p, "\"Filter\": \"(x > 5)\"")) << p;
        EXPECT_TRUE(has(p, "\"Relation Name\": \"t\"")) << p;
        EXPECT_TRUE(has(p, "\"Planning Time\"") && has(p, "\"Translation Time\"")) << p;
        EXPECT_FALSE(has(p, "Actual Rows") || has(p, "Execution Time")) << p;
        // without ANALYZE, a write is shown, not done
        const std::string u = plan("explain UPDATE t SET x = 0 WHERE g = 1;");
        EXPECT_TRUE(has(u, "\"Node Type\": \"Update\"") && has(u, "\"Set\": [\"x = 0\"]")) << u;
        EXPECT_EQ(rows("SELECT x FROM t WHERE g = 1;"), (std::vector<std::string>{"(10)", "(20)"}));
    }

    TEST_F(ExplainTest, AnalyzeCountsRows) {
        const size_t threads = Parallel::threads, morselSize = Parallel::morselSize;
        for (size_t t : {threads, size_t(4)}) {
            Parallel::threads = t;
            Parallel::morselSize = (t == threads) ? morselSize : 2;
            // result, join, scan of u (the hash table of the smaller table), filter, scan of t
            const std::string p = plan("EXPLAIN ANALYZE SELECT t.name, u.y FROM t, u WHERE t.g = u.g AND x > 5;");
            EXPECT_TRUE(has(p, "\"Join Cond\": \"(t.g = u.g)\"")) << p;
            EXPECT_EQ(actualRows(p), (std::vector<int>{3, 3, 2, 3, 4})) << p;
            EXPECT_TRUE(has(p, "\"Compilation Time\"") && has(p, "\"Execution Time\"")) << p;
        }
        Parallel::threads = threads;
        Parallel::morselSize = morselSize;
        // with ANALYZE, a write is done
        const std::string u = plan("EXPLAIN (ANALYZE true, FORMAT JSON) UPDATE t SET x = x + 1 WHERE g = 1;");
        EXPECT_EQ(actualRows(u), (std::vector<int>{2, 4})) << u; // the filter and the scan
        EXPECT_EQ(rows("SELECT x FROM t WHERE g = 1;"), (std::vector<std::string>{"(11)", "(21)"}));
    }

    TEST_F(ExplainTest, SeveralQueries) {
        // GROUP BY in the plan: a hash aggregation, HAVING a filter over its groups
        const std::string g = plan("EXPLAIN ANALYZE SELECT g, count(*) FROM t GROUP BY g HAVING count(*) > 1;");
        EXPECT_TRUE(has(g, "\"Node Type\": \"HashAggregate\"") && has(g, "\"Group Key\": [\"g\"]")) << g;
        EXPECT_EQ(actualRows(g), (std::vector<int>{1, 1, 3, 4})) << g;
        // also of varchar, as text; without ANALYZE
        const std::string a = plan("EXPLAIN SELECT g, max(CAST(name AS varchar(5))) FROM t GROUP BY g HAVING count(*) > 1;");
        EXPECT_TRUE(has(a, "\"Node Type\": \"HashAggregate\"") && !has(a, "\"Queries\"")) << a;
        // a recursive common table expression: one plan, whose loop runs the steps over the working table
        const std::string r = plan("EXPLAIN ANALYZE WITH RECURSIVE r(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM r WHERE i < 3) SELECT max(i) FROM r;");
        EXPECT_TRUE(has(r, "\"Node Type\": \"Recursive Union\"") && has(r, "\"Node Type\": \"WorkTable Scan\"")) << r;
        // over several branches
        exec("CREATE BRANCH b FROM master;");
        const std::string b = plan("EXPLAIN SELECT branch, sum(x) FROM t VERSION ALL GROUP BY branch;");
        EXPECT_TRUE(has(b, "\"Node Type\": \"Branch Aggregation\"") && has(b, "\"Aggregates\": [\"sum(x)\"]")) << b;
    }

    TEST_F(ExplainTest, Errors) {
        EXPECT_THROW(exec("EXPLAIN CREATE TABLE z ( a integer );"), std::exception);
        EXPECT_THROW(exec("EXPLAIN DROP BRANCH master;"), std::exception);
        EXPECT_THROW(exec("EXPLAIN (FORMAT TEXT) SELECT g FROM t;"), std::exception);
        EXPECT_THROW(exec("EXPLAIN (BUFFERS) SELECT g FROM t;"), std::exception);
        EXPECT_THROW(exec("EXPLAIN EXPLAIN SELECT g FROM t;"), std::exception);
        EXPECT_THROW(exec("EXPLAIN;"), std::exception);
        EXPECT_THROW(exec("EXPLAIN SELECT 1 UNION SELECT 2;"), std::exception);
        EXPECT_FALSE(db->hasTable("z"));
        EXPECT_EQ(rows("SELECT count(*) FROM t;"), (std::vector<std::string>{"(4)"}));
    }

    TEST_F(ExplainTest, Timing) {
        auto result = exec("SELECT g, x FROM t WHERE x > 5;");
        const auto & t = result.timing;
        EXPECT_GT(t.total, 0);
        EXPECT_GT(t.llvm, 0);
        EXPECT_GT(t.parsing, 0);
        EXPECT_GE(t.execution, 0);
        EXPECT_NEAR(t.compilation() + t.execution, t.total, 1e-6 + t.total * 1e-9);
        // the queries of a statement add up
        auto aggregate = exec("SELECT g, count(*) FROM t GROUP BY g;");
        EXPECT_GT(aggregate.timing.llvm, 0);
        EXPECT_LE(aggregate.timing.compilation(), aggregate.timing.total);
        // compileAndExecute (the shell) returns them as well
        testing::internal::CaptureStdout();
        auto shell = QueryCompiler::compileAndExecute("SELECT g FROM t;", *db);
        testing::internal::GetCapturedStdout();
        EXPECT_GT(shell.total, 0);
        EXPECT_GT(shell.llvm, 0);
    }

}
