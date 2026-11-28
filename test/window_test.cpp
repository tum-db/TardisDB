// Window functions (OVER, WINDOW): ranking, navigation and aggregates over partitions and frames, as PostgreSQL computes them, also in parallel; errors

#include "codegen/LLVMCompat.hpp"

#include <algorithm>
#include <mutex>

#include "foundations/Parallel.hpp"
#include "runtime/SqlTuple.hpp"
#include "queryCompiler/queryCompiler.hpp"
#include <gtest/gtest.h>

namespace {

    using rows_t = std::vector<std::string>;

    class WindowTest : public ::testing::Test {
    protected:
        void SetUp() override {
            db = std::make_unique<Database>();
            exec("CREATE TABLE t ( id integer, g integer, v integer, x double precision, s text );");
            exec("INSERT INTO t VALUES (1, 1, 10, 1.5, 'a'), (2, 1, 20, 2.5, 'b'), (3, 1, 20, NULL, 'c'), (4, 2, 5, 4.0, 'd'), "
                 "(5, 2, NULL, 5.0, NULL), (6, NULL, 7, 6.0, 'f'), (7, 1, -3, 0.5, 'g'), (8, 2, 5, 4.5, 'h');");
        }

        QueryCompiler::StatementResult exec(const std::string & sql) {
            return QueryCompiler::execute(sql, *db, "master", [](Native::Sql::SqlTuple *) { });
        }

        rows_t query(const std::string & sql) {
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

        std::unique_ptr<Database> db;
    };

    // the expected rows are those of PostgreSQL

    TEST_F(WindowTest, Ranking) {
        EXPECT_EQ(query("SELECT id, row_number() OVER (PARTITION BY g ORDER BY v DESC NULLS LAST, id), rank() OVER w, dense_rank() OVER w, "
                        "percent_rank() OVER w, cume_dist() OVER w FROM t WINDOW w AS (PARTITION BY g ORDER BY v) ORDER BY id;"),
                  (rows_t{"(1, 3, 2, 2, 0.3333333333333333, 0.5)", "(2, 1, 3, 3, 0.6666666666666666, 1)", "(3, 2, 3, 3, 0.6666666666666666, 1)",
                          "(4, 1, 1, 1, 0, 0.6666666666666666)", "(5, 3, 3, 2, 1, 1)", "(6, 1, 1, 1, 0, 1)", "(7, 4, 1, 1, 0, 0.25)",
                          "(8, 2, 1, 1, 0, 0.6666666666666666)"}));
        // one operator per partitioning and ordering
        const std::string p = plan("SELECT rank() OVER (ORDER BY v), dense_rank() OVER (ORDER BY v), row_number() OVER (PARTITION BY g) FROM t;");
        size_t operators = 0;
        for (size_t at = p.find("\"WindowAgg\""); at != std::string::npos; at = p.find("\"WindowAgg\"", at + 1)) ++operators;
        EXPECT_EQ(operators, 2u) << p;
    }

    TEST_F(WindowTest, Navigation) {
        EXPECT_EQ(query("SELECT id, ntile(3) OVER (ORDER BY id), lag(v) OVER (ORDER BY id), lead(v, 2, 0) OVER (PARTITION BY g ORDER BY id), "
                        "lead(s, 1, 'z') OVER (ORDER BY id) FROM t ORDER BY id;"),
                  (rows_t{"(1, 1, NULL, 20, b)", "(2, 1, 10, -3, c)", "(3, 1, 20, 0, d)", "(4, 2, 20, 5, NULL)", "(5, 2, 5, 0, f)",
                          "(6, 2, NULL, 0, g)", "(7, 3, 7, 0, h)", "(8, 3, -3, 0, z)"}));
        EXPECT_EQ(query("SELECT id, first_value(v) OVER w, last_value(v) OVER w, nth_value(s, 2) OVER w FROM t WINDOW w AS (PARTITION BY g ORDER BY id) ORDER BY id;"),
                  (rows_t{"(1, 10, 10, NULL)", "(2, 10, 20, b)", "(3, 10, 20, b)", "(4, 5, 5, NULL)", "(5, 5, NULL, NULL)", "(6, 7, 7, NULL)",
                          "(7, 10, -3, b)", "(8, 5, 5, NULL)"}));
        // the rate of change of the paper
        EXPECT_EQ(query("SELECT id, (x - lag(x) OVER w) / (id - lag(id) OVER w) FROM t WINDOW w AS (ORDER BY id) ORDER BY id;"),
                  (rows_t{"(1, NULL)", "(2, 1)", "(3, NULL)", "(4, NULL)", "(5, 1)", "(6, 1)", "(7, -5.5)", "(8, 4)"}));
    }

    TEST_F(WindowTest, Aggregates) {
        EXPECT_EQ(query("SELECT id, sum(v) OVER w, count(v) OVER w, count(*) OVER w, min(s) OVER w, max(x) OVER w, avg(x) OVER w FROM t "
                        "WINDOW w AS (PARTITION BY g) ORDER BY id;"),
                  (rows_t{"(1, 47, 4, 4, a, 2.5, 1.5)", "(2, 47, 4, 4, a, 2.5, 1.5)", "(3, 47, 4, 4, a, 2.5, 1.5)", "(4, 10, 2, 3, d, 5, 4.5)",
                          "(5, 10, 2, 3, d, 5, 4.5)", "(6, 7, 1, 1, f, 6, 6)", "(7, 47, 4, 4, a, 2.5, 1.5)", "(8, 10, 2, 3, d, 5, 4.5)"}));
        // over the groups of GROUP BY, and filtered in a subquery (the medals of the paper)
        EXPECT_EQ(query("SELECT g, sum(v), rank() OVER (ORDER BY sum(v) DESC), sum(sum(v)) OVER (ORDER BY g) FROM t GROUP BY g ORDER BY g;"),
                  (rows_t{"(1, 47, 1, 47)", "(2, 10, 2, 57)", "(NULL, 7, 3, 64)"}));
        EXPECT_EQ(query("SELECT * FROM (SELECT id, rank() OVER (ORDER BY x DESC NULLS LAST) AS medal FROM t) r WHERE medal <= 3 ORDER BY id;"),
                  (rows_t{"(5, 2)", "(6, 1)", "(8, 3)"}));
        EXPECT_EQ(query("SELECT id FROM t ORDER BY row_number() OVER (ORDER BY v DESC NULLS LAST, id) LIMIT 3;"), (rows_t{"(2)", "(3)", "(1)"}));
    }

    TEST_F(WindowTest, Frames) {
        EXPECT_EQ(query("SELECT id, sum(v) OVER (ORDER BY id ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING), count(*) OVER (ORDER BY id ROWS BETWEEN 2 PRECEDING "
                        "AND 1 PRECEDING), max(x) OVER (ORDER BY id ROWS BETWEEN 1 FOLLOWING AND 3 FOLLOWING) FROM t ORDER BY id;"),
                  (rows_t{"(1, 30, 0, 4)", "(2, 50, 1, 5)", "(3, 45, 2, 6)", "(4, 25, 2, 6)", "(5, 12, 2, 6)", "(6, 4, 2, 4.5)", "(7, 9, 2, 4.5)", "(8, 2, 2, NULL)"}));
        EXPECT_EQ(query("SELECT id, sum(v) OVER (ORDER BY v RANGE BETWEEN 5 PRECEDING AND 5 FOLLOWING), count(*) OVER (ORDER BY x DESC RANGE BETWEEN 1 PRECEDING "
                        "AND CURRENT ROW), sum(v) OVER (ORDER BY v GROUPS BETWEEN 1 PRECEDING AND 1 FOLLOWING) FROM t ORDER BY id;"),
                  (rows_t{"(1, 27, 2, 57)", "(2, 40, 1, 50)", "(3, 40, 1, 50)", "(4, 27, 3, 14)", "(5, NULL, 2, 40)", "(6, 27, 1, 27)", "(7, -3, 2, 7)", "(8, 27, 2, 14)"}));
        // bounds of the rows themselves (which PostgreSQL does not take)
        EXPECT_EQ(query("SELECT id, sum(id) OVER (ORDER BY id ROWS BETWEEN id - 6 PRECEDING AND CURRENT ROW) FROM t WHERE id > 5 ORDER BY id;"),
                  (rows_t{"(6, 6)", "(7, 13)", "(8, 21)"}));
    }

    TEST_F(WindowTest, Parallel) {
        // hash groups of the workers, a sort and the functions within large partitions, segment trees; the sums of PostgreSQL
        exec("CREATE TABLE r ( id integer, g integer, v integer );");
        exec("INSERT INTO r SELECT i, i - 3 * (i / 3), i * 7919 - 1000 * ((i * 7919) / 1000) FROM generate_series(1, 60000) AS s(i);");
        const std::vector<std::pair<std::string, std::string>> queries = {
            {"SELECT sum(r), sum(dr), sum(n), count(*) FROM (SELECT rank() OVER (PARTITION BY g ORDER BY v) AS r, dense_rank() OVER (PARTITION BY g ORDER BY v) AS dr, "
             "ntile(7) OVER (PARTITION BY g ORDER BY v, id) AS n FROM r) s;", "(599460000, 30030000, 239991, 60000)"},
            {"SELECT sum(w), sum(m) FROM (SELECT sum(v) OVER (ORDER BY v, id ROWS BETWEEN 100 PRECEDING AND 50 FOLLOWING) AS w, "
             "min(v) OVER (ORDER BY id ROWS BETWEEN 1000 PRECEDING AND CURRENT ROW) AS m FROM r) s;", "(4520425870, 8280)"},
            {"SELECT sum(r), sum(l), sum(f) FROM (SELECT row_number() OVER (PARTITION BY v ORDER BY id) AS r, lag(id, 2, 0) OVER (PARTITION BY v ORDER BY id) AS l, "
             "first_value(id) OVER (PARTITION BY v ORDER BY id DESC) AS f FROM r) s;", "(1830000, 1682029000, 3570030000)"},
            {"SELECT sum(c), sum(w), sum(k) FROM (SELECT count(*) OVER (ORDER BY v RANGE BETWEEN 5 PRECEDING AND 5 FOLLOWING) AS c, sum(v) OVER (PARTITION BY g "
             "ORDER BY v GROUPS BETWEEN 1 FOLLOWING AND 3 FOLLOWING) AS w, sum(v) OVER (ORDER BY id) AS k FROM r) s;", "(39492000, 1798195200, 899146980000)"}};
        const size_t threads = Parallel::threads, morselSize = Parallel::morselSize;
        for (size_t workers : {size_t(1), size_t(4)}) {
            Parallel::threads = workers;
            Parallel::morselSize = 1000;
            for (auto & [sql, sums] : queries) EXPECT_EQ(query(sql), rows_t{sums}) << workers << " threads: " << sql;
        }
        Parallel::threads = threads;
        Parallel::morselSize = morselSize;
    }

    TEST_F(WindowTest, Errors) {
        const std::vector<std::string> wrong = {
            "SELECT id FROM t WHERE rank() OVER (ORDER BY v) > 1;",
            "SELECT g, count(*) FROM t GROUP BY g HAVING rank() OVER (ORDER BY g) > 1;",
            "SELECT sum(rank() OVER (ORDER BY v)) OVER () FROM t;",
            "SELECT rank() OVER w FROM t;",
            "SELECT median(v) OVER () FROM t;",
            "SELECT rank(v) OVER () FROM t;",
            "SELECT ntile(0) OVER () FROM t;",
            "SELECT sum(v) OVER (ORDER BY id ROWS BETWEEN UNBOUNDED FOLLOWING AND CURRENT ROW) FROM t;",
            "SELECT sum(v) OVER (ORDER BY id ROWS BETWEEN CURRENT ROW AND 1 PRECEDING) FROM t;",
            "SELECT sum(v) OVER (ORDER BY id ROWS BETWEEN -1 PRECEDING AND CURRENT ROW) FROM t;",
            "SELECT sum(v) OVER (ORDER BY id, v RANGE BETWEEN 1 PRECEDING AND CURRENT ROW) FROM t;",
            "SELECT sum(v) OVER (ORDER BY s RANGE BETWEEN 1 PRECEDING AND CURRENT ROW) FROM t;",
            "SELECT sum(v) OVER (GROUPS BETWEEN 1 PRECEDING AND CURRENT ROW) FROM t;",
            "SELECT sum(v) OVER (ORDER BY id ROWS BETWEEN 1 PRECEDING AND CURRENT ROW EXCLUDE CURRENT ROW) FROM t;",
            "SELECT sum(s) OVER () FROM t;",
            "SELECT rank() OVER w FROM t WINDOW w AS (ORDER BY v), w AS (ORDER BY id);"};
        for (auto & sql : wrong) EXPECT_THROW(exec(sql), std::exception) << sql;
        EXPECT_EQ(query("SELECT count(*) FROM t;"), (rows_t{"(8)"}));
    }

} // namespace
