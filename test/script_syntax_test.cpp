// The syntax of SQL scripts: operators without spaces, CREATE TABLE IF NOT EXISTS, COPY in the text format,
// LIMIT and OFFSET, INSERT ... SELECT, random(), * among other items, subqueries in FROM, ARRAY of array
// expressions, and gradient descent in recursive queries over them

#include "codegen/LLVMCompat.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <unistd.h>
#include "native/sql/SqlTuple.hpp"
#include "queryCompiler/queryCompiler.hpp"
#include "semanticAnalyser/SemanticAnalyser.hpp"
#include <gtest/gtest.h>

namespace {

    using rows_t = std::vector<std::string>;

    class ScriptSyntaxTest : public ::testing::Test {
    protected:
        void SetUp() override {
            db = std::make_unique<Database>();
            exec("CREATE TABLE st ( a integer not null, b double precision );");
            exec("INSERT INTO st VALUES (1, 2.5), (2, 3.5), (3, NULL);");
        }

        QueryCompiler::StatementResult exec(const std::string & sql) {
            return QueryCompiler::execute(sql, *db, "master", [](Native::Sql::SqlTuple *) { });
        }

        /// \returns The rows in the order of the result
        rows_t run(const std::string & sql) {
            rows_t rows;
            std::mutex mutex;
            QueryCompiler::execute(sql, *db, "master", [&](Native::Sql::SqlTuple * tuple) {
                std::lock_guard<std::mutex> lock(mutex);
                rows.push_back(Native::Sql::toString(*tuple));
            });
            return rows;
        }

        rows_t sorted(const std::string & sql) {
            rows_t rows = run(sql);
            std::sort(rows.begin(), rows.end());
            return rows;
        }

        std::string file(const std::string & name, const std::string & content) {
            std::string path = "/tmp/tardisdb_script_syntax_" + std::to_string(getpid()) + "_" + name;
            std::ofstream(path) << content;
            files.push_back(path);
            return path;
        }

        void TearDown() override { for (auto & path : files) std::remove(path.c_str()); }

        /// \returns The numbers of a row "(1, 2.5, ...)"
        static std::vector<double> numbers(const std::string & row) {
            std::vector<double> result;
            std::stringstream in(row.substr(1, row.size() - 2));
            for (std::string item; std::getline(in, item, ',');) result.push_back(std::stod(item));
            return result;
        }

        std::unique_ptr<Database> db;
        std::vector<std::string> files;
    };

    TEST_F(ScriptSyntaxTest, Operators) {
        // + - / need no spaces; a sign after no operand belongs to the number, as the sign of an exponent
        EXPECT_EQ(sorted("SELECT a+1, a-1, b/2, 2-a, a*2-1 FROM st WHERE a<3;"), (rows_t{"(2, 0, 1.25, 1, 1)", "(3, 1, 1.75, 0, 3)"}));
        EXPECT_EQ(run("SELECT -5, 3 - -2, 1e-1*10, 2.5E+1;"), (rows_t{"(-5, 5, 1, 25)"}));
        EXPECT_EQ(run("SELECT 3--2;"), (rows_t{"(3)"})); // -- begins a comment, as in PostgreSQL
        EXPECT_EQ(sorted("SELECT a FROM st WHERE a=-1+3;"), (rows_t{"(2)"}));
        exec("INSERT INTO st VALUES (-4, -1.5);");
        EXPECT_EQ(run("SELECT a, b FROM st WHERE a<0;"), (rows_t{"(-4, -1.5)"}));
        // comments to the end of the line
        EXPECT_EQ(run("SELECT a -- the column\n FROM st WHERE a = 1;"), (rows_t{"(1)"}));
    }

    TEST_F(ScriptSyntaxTest, CreateCopy) {
        exec("CREATE TABLE IF NOT EXISTS st ( x integer );");
        EXPECT_EQ(run("SELECT count(*) FROM st;"), (rows_t{"(3)"}));
        exec("CREATE TABLE IF NOT EXISTS ct ( n integer, s text );");
        // the text format: the default, also in the older form without a format; \N is NULL, backslash escapes
        exec("COPY ct FROM '" + file("a.txt", "1\ta\\tb\n2\t\\N\n\\.\n9\tafter the end\n") + "';");
        exec("COPY ct FROM '" + file("b.csv", "n,s\n3,c\n") + "' DELIMITER ',' HEADER;");
        EXPECT_EQ(sorted("SELECT n, s FROM ct;"), (rows_t{"(1, a\tb)", "(2, NULL)", "(3, c)"}));
        const std::string out = file("out.txt", "");
        exec("COPY ct TO '" + out + "' (FORMAT text, NULL 'none');");
        std::ifstream in(out);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        EXPECT_NE(text.find("1\ta\\tb\n"), std::string::npos) << text;
        EXPECT_NE(text.find("2\tnone\n"), std::string::npos) << text;
    }

    TEST_F(ScriptSyntaxTest, LimitOffset) {
        EXPECT_EQ(run("SELECT a FROM st ORDER BY a DESC LIMIT 2;"), (rows_t{"(3)", "(2)"}));
        EXPECT_EQ(run("SELECT a FROM st ORDER BY a LIMIT 1 OFFSET 1;"), (rows_t{"(2)"}));
        EXPECT_EQ(run("SELECT a FROM st ORDER BY a OFFSET 2 ROWS;"), (rows_t{"(3)"}));
        EXPECT_EQ(run("SELECT a FROM st LIMIT 2;").size(), 2u);
        EXPECT_EQ(run("SELECT a FROM st ORDER BY a LIMIT ALL;").size(), 3u);
        EXPECT_EQ(run("SELECT * FROM generate_series(1, 9) LIMIT 1 + 1;").size(), 2u);
        EXPECT_EQ(run("SELECT a FROM st UNION ALL SELECT a FROM st ORDER BY 1 LIMIT 3;"), (rows_t{"(1)", "(1)", "(2)"}));
        EXPECT_THROW(exec("SELECT a FROM st LIMIT -1;"), std::exception);
        EXPECT_THROW(exec("SELECT a FROM st LIMIT 1 LIMIT 2;"), std::exception);
    }

    TEST_F(ScriptSyntaxTest, InsertSelect) {
        exec("CREATE TABLE it ( x double precision, y double precision, s text );");
        exec("INSERT INTO it SELECT a, b, 'it''s' FROM st WHERE a = 1;");
        exec("INSERT INTO it (SELECT g, g * 2, NULL FROM generate_series(2, 3) g);");
        exec("INSERT INTO it (y, x) SELECT 7, 8;");
        EXPECT_EQ(exec("INSERT INTO it SELECT x, y, s FROM it WHERE x > 100;").rows, 0u);
        EXPECT_EQ(sorted("SELECT x, y, s FROM it;"), (rows_t{"(1, 2.5, it's)", "(2, 4, NULL)", "(3, 6, NULL)", "(8, 7, NULL)"}));
    }

    TEST_F(ScriptSyntaxTest, InsertSelectInOnePlan) {
        // the insert writes the rows of the query in its plan
        exec("CREATE TABLE ip ( k integer not null, n numeric(6,2), v varchar(4), t text, a double precision[] );");
        std::string p;
        QueryCompiler::execute("EXPLAIN INSERT INTO ip SELECT a, b, 'x', 'y', NULL FROM st;", *db, "master",
                               [&](Native::Sql::SqlTuple * tuple) { p = Native::Sql::toString(*tuple); });
        EXPECT_NE(p.find("\"Node Type\": \"Insert\""), std::string::npos) << p;
        EXPECT_NE(p.find("\"Relation Name\": \"st\""), std::string::npos) << p;
        EXPECT_EQ(run("SELECT count(*) FROM ip;"), rows_t{"(0)"});
        // casts to the columns, computed texts and arrays outlive the query, the other columns NULL
        EXPECT_EQ(exec("INSERT INTO ip SELECT a * 10, b, array_to_string(array[a, a * 7], '-'), array_to_string(array[a, a, a, a], ' and '), array[b, a] * 2 "
                       "FROM st;").rows, 3u);
        EXPECT_EQ(exec("INSERT INTO ip (t, k) SELECT 'w', count(*) FROM st;").rows, 1u);
        EXPECT_EQ(sorted("SELECT * FROM ip;"), (rows_t{"(10, 2.50, 1-7, 1 and 1 and 1 and 1, {5,2})", "(20, 3.50, 2-14, 2 and 2 and 2 and 2, {7,4})",
                                                        "(3, NULL, NULL, w, NULL)", "(30, NULL, 3-21, 3 and 3 and 3 and 3, {NULL,6})"}));
        // NULL into a column without NULLs, when it comes
        EXPECT_THROW(exec("INSERT INTO ip (k) SELECT NULL FROM st;"), semanticalAnalysis::not_null_violation);
        EXPECT_THROW(exec("INSERT INTO ip (k) SELECT b FROM st;"), semanticalAnalysis::not_null_violation);
        EXPECT_EQ(exec("INSERT INTO ip (k) SELECT b FROM st WHERE b IS NOT NULL;").rows, 2u);
        EXPECT_THROW(exec("INSERT INTO ip (k, n) SELECT a FROM st;"), std::exception);
        EXPECT_THROW(exec("INSERT INTO ip (k) SELECT a, b FROM st;"), std::exception);
        EXPECT_THROW(exec("INSERT INTO ip (nope) SELECT a FROM st;"), std::exception);
        // into a branch, from a subquery with an aggregate joined with a table function
        exec("CREATE BRANCH side FROM master;");
        EXPECT_EQ(exec("INSERT INTO ip VERSION side (k) SELECT g + s.a + 100 FROM (SELECT max(a) AS a FROM st) s, generate_series(1, 2) g;").rows, 2u);
        EXPECT_EQ(run("SELECT k FROM ip VERSION side WHERE k > 100 ORDER BY k;"), (rows_t{"(104)", "(105)"}));
        EXPECT_EQ(run("SELECT count(*) FROM ip WHERE k > 100;"), rows_t{"(0)"});
        // the rows of the other queries, and of a query of the table itself, are inserted after it
        EXPECT_EQ(exec("INSERT INTO st SELECT a + 10, b FROM st;").rows, 3u);
        EXPECT_EQ(exec("INSERT INTO st (a) SELECT a FROM st ORDER BY a LIMIT 2;").rows, 2u);
        EXPECT_EQ(exec("INSERT INTO st (a) WITH w AS (SELECT 100 AS a) SELECT a FROM w UNION ALL SELECT 101;").rows, 2u);
        EXPECT_EQ(run("SELECT count(*), sum(a) FROM st;"), rows_t{"(10, 246)"});
    }

    TEST_F(ScriptSyntaxTest, StarRandom) {
        EXPECT_EQ(sorted("SELECT *, a * 10 FROM st WHERE a < 3;"), (rows_t{"(1, 2.5, 10)", "(2, 3.5, 20)"}));
        EXPECT_EQ(sorted("SELECT a + 1 AS c, * FROM st WHERE a = 1;"), (rows_t{"(2, 1, 2.5)"}));
        // random(): another value in [0, 1) for each row
        EXPECT_EQ(run("SELECT count(*), min(r) >= 0, max(r) < 1, count(*) > 990 FROM (SELECT random() AS r FROM generate_series(1, 1000)) s;"),
                  (rows_t{"(1000, 1, 1, 1)"}));
        const rows_t values = sorted("SELECT random() FROM generate_series(1, 100);");
        EXPECT_GT(std::set<std::string>(values.begin(), values.end()).size(), 90u);
        EXPECT_THROW(exec("SELECT random(1);"), std::exception);
    }

    TEST_F(ScriptSyntaxTest, Subqueries) {
        EXPECT_EQ(run("SELECT x FROM (SELECT a AS x FROM st) s ORDER BY x;"), (rows_t{"(1)", "(2)", "(3)"}));
        EXPECT_EQ(run("SELECT * FROM (SELECT a, b * 2 FROM st WHERE a > 1) AS s(p, q) ORDER BY p;"), (rows_t{"(2, 7)", "(3, NULL)"}));
        // without an alias, nested, in joins, with * and aggregates
        EXPECT_EQ(run("SELECT count(*) FROM (SELECT * FROM (SELECT a FROM st) u);"), (rows_t{"(3)"}));
        EXPECT_EQ(run("SELECT s.a, st.b FROM (SELECT a FROM st WHERE a < 3) s, st WHERE s.a = st.a ORDER BY 1;"), (rows_t{"(1, 2.5)", "(2, 3.5)"}));
        EXPECT_EQ(run("SELECT c * 2, * FROM (SELECT a + 1 AS c, * FROM st WHERE a = 2) t;"), (rows_t{"(6, 3, 2, 3.5)"}));
        EXPECT_EQ(run("SELECT sum(x) FROM (SELECT a * 10 AS x FROM st) s;"), (rows_t{"(60)"}));
        EXPECT_THROW(exec("SELECT * FROM (SELECT a, a FROM st) s;"), std::exception);
        EXPECT_THROW(exec("SELECT * FROM (SELECT a FROM st) s(x, y);"), std::exception);
    }

    TEST_F(ScriptSyntaxTest, Arrays) {
        // ARRAY of array expressions has one dimension more
        exec("CREATE TABLE ar ( k integer );");
        exec("INSERT INTO ar VALUES (0), (2);");
        EXPECT_EQ(sorted("SELECT array[(array_fill(0::float, array[k]) || 1::float) || array_fill(0::float, array[2-k])] FROM ar;"),
                  (rows_t{"({{0,0,1}})", "({{1,0,0}})"}));
        EXPECT_EQ(sorted("SELECT array[[k/2, k+1]] FROM ar;"), (rows_t{"({{0,1}})", "({{1,3}})"}));
        EXPECT_EQ(sorted("SELECT array[array[1, 2], array[k, 3]] FROM ar;"), (rows_t{"({{1,2},{0,3}})", "({{1,2},{2,3}})"}));
    }

    TEST_F(ScriptSyntaxTest, GradientDescent) {
        // linear regression in a recursive query, its aggregates over a join with the step
        exec("CREATE TABLE data ( x float, y float );");
        exec("INSERT INTO data (SELECT *, 5*x+10 FROM (SELECT g::float / 100 x FROM generate_series(1, 100) g));");
        const rows_t rows = run("WITH RECURSIVE gd (id, a, b) AS ( SELECT 1, 10::float, 10::float UNION ALL "
                                "SELECT id+1, a-0.05*avg(2*x*(a*x+b-y)), b-0.05*avg(2*(a*x+b-y)) FROM gd, data WHERE id < 4 GROUP BY id, a, b) "
                                "SELECT * FROM gd ORDER BY id;");
        ASSERT_EQ(rows.size(), 4u);
        double a = 10, b = 10;
        for (size_t step = 0; step < rows.size(); ++step) {
            const std::vector<double> row = numbers(rows[step]);
            EXPECT_EQ(row[0], step + 1);
            EXPECT_NEAR(row[1], a, 1e-12);
            EXPECT_NEAR(row[2], b, 1e-12);
            double da = 0, db = 0;
            for (int g = 1; g <= 100; ++g) {
                const double x = g / 100.0, e = a * x + b - (5 * x + 10);
                da += 2 * x * e / 100;
                db += 2 * e / 100;
            }
            a -= 0.05 * da;
            b -= 0.05 * db;
        }
        // a neural network: subqueries in FROM that read the step, arrays and matrix products
        exec("CREATE TABLE nn ( img float[], one_hot float[] );");
        exec("INSERT INTO nn VALUES ('{{0.5,0.1}}', '{{1,0}}'), ('{{0.1,0.6}}', '{{0,1}}');");
        const rows_t steps = run("WITH RECURSIVE gd (id, w) AS ( SELECT 0, '{{0.1,-0.2},{0.3,0.4}}'::float[] UNION ALL "
                                 "SELECT id+1, w - 0.5 * sum(transpose(img)**d) FROM ( SELECT (2*(a-one_hot)) * a * (1-a) AS d, * FROM ( "
                                 "SELECT sig(img**w) AS a, * FROM (SELECT * FROM nn), gd WHERE id < 2)) GROUP BY id, w) SELECT w FROM gd ORDER BY id;");
        ASSERT_EQ(steps.size(), 3u);
        // the same steps: w -= 0.5 * sum of img^T (2 (a - one_hot) a (1 - a)), a = sig(img w)
        double w[2][2] = {{0.1, -0.2}, {0.3, 0.4}};
        const double img[2][2] = {{0.5, 0.1}, {0.1, 0.6}}, hot[2][2] = {{1, 0}, {0, 1}};
        for (int step = 0; step < 2; ++step) {
            double grad[2][2] = {{0, 0}, {0, 0}};
            for (int r = 0; r < 2; ++r) {
                for (int j = 0; j < 2; ++j) {
                    const double z = img[r][0] * w[0][j] + img[r][1] * w[1][j], act = 1 / (1 + std::exp(-z));
                    const double d = 2 * (act - hot[r][j]) * act * (1 - act);
                    for (int i = 0; i < 2; ++i) grad[i][j] += img[r][i] * d;
                }
            }
            for (int i = 0; i < 2; ++i) for (int j = 0; j < 2; ++j) w[i][j] -= 0.5 * grad[i][j];
        }
        std::string last = steps.back();
        last.erase(std::remove(last.begin(), last.end(), '{'), last.end());
        last.erase(std::remove(last.begin(), last.end(), '}'), last.end());
        const std::vector<double> got = numbers(last);
        ASSERT_EQ(got.size(), 4u) << steps.back();
        for (int k = 0; k < 4; ++k) EXPECT_NEAR(got[k], w[k / 2][k % 2], 1e-12);
    }

} // namespace
