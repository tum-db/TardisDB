// derivation(TABLE (query), lambda (x) (expression)) against finite differences, over numbers and arrays, in gradient descent; exp, ln, log, sqrt, sin, cos

#include "codegen/LLVMCompat.hpp"

#include <cmath>
#include <functional>
#include <mutex>
#include <regex>

#include "runtime/SqlTuple.hpp"
#include "queryCompiler/queryCompiler.hpp"
#include <gtest/gtest.h>

namespace {

    using rows_t = std::vector<std::string>;

    class DerivationTest : public ::testing::Test {
    protected:
        void SetUp() override {
            db = std::make_unique<Database>();
            exec("CREATE TABLE p ( a double precision, b double precision, x double precision, y double precision );");
            exec("INSERT INTO p VALUES (2, 1, 3, 4);");
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

        /// The numbers of the only row of the query
        std::vector<double> numbers(const std::string & sql) {
            rows_t rows = query(sql);
            EXPECT_EQ(rows.size(), 1u) << sql;
            std::vector<double> values;
            if (rows.empty()) return values;
            static const std::regex number(R"(-?[0-9][-0-9.e+]*)");
            for (std::sregex_iterator it(rows[0].begin(), rows[0].end(), number), end; it != end; ++it) values.push_back(std::stod(it->str()));
            return values;
        }

        std::unique_ptr<Database> db;
    };

    TEST_F(DerivationTest, Numbers) {
        // the rows of the query, then d_<c> per column of the parameter in the order of their first use
        EXPECT_EQ(query("SELECT * FROM derivation(TABLE (SELECT * FROM p), lambda (x) ((x.a * x.x + x.b - x.y)^2));"),
                  (rows_t{"(2, 1, 3, 4, 18, 12, 6, -6)"}));
        EXPECT_EQ(query("SELECT d_a FROM derivation(TABLE (SELECT a, b FROM p), lambda (r) (r.a * r.a * b)) d;"), (rows_t{"(4)"}));
        // each function against a central difference at a = 0.7 (and b = 1.3)
        const std::vector<std::pair<std::string, std::function<double(double, double)>>> functions = {
            {"x.a - x.b / x.a", [](double a, double b) { return a - b / a; }},
            {"- x.a * x.b", [](double a, double b) { return -a * b; }},
            {"x.a ^ x.b", [](double a, double b) { return std::pow(a, b); }},
            {"x.a ^ 3", [](double a, double) { return std::pow(a, 3); }},
            {"sig(x.a * x.b)", [](double a, double b) { return 1 / (1 + std::exp(-a * b)); }},
            {"tanh(x.a) + relu(x.b - x.a) + abs(x.a - x.b) + (@ x.b)", [](double a, double b) { return std::tanh(a) + std::max(b - a, 0.0) + std::fabs(a - b) + std::fabs(b); }},
            {"exp(x.a) * ln(x.b)", [](double a, double b) { return std::exp(a) * std::log(b); }},
            {"log(x.a) + log(x.b, x.a + 2)", [](double a, double b) { return std::log10(a) + std::log(a + 2) / std::log(b); }},
            {"sqrt(x.a) / sin(x.b) + cos(x.a * x.b)", [](double a, double b) { return std::sqrt(a) / std::sin(b) + std::cos(a * b); }},
            {"CAST(x.a AS double precision) * 2", [](double a, double) { return a * 2; }},
        };
        const double a = 0.7, b = 1.3, h = 1e-6;
        for (auto & [body, f] : functions) {
            const auto d = numbers("SELECT d_a, d_b FROM derivation(TABLE (SELECT 0.7::float AS a, 1.3::float AS b), lambda (x) (" + body +
                                   " + 0 * x.b));");
            ASSERT_EQ(d.size(), 2u) << body;
            EXPECT_NEAR(d[0], (f(a + h, b) - f(a - h, b)) / (2 * h), 1e-6) << body;
            EXPECT_NEAR(d[1], (f(a, b + h) - f(a, b - h)) / (2 * h), 1e-6) << body;
        }
    }

    TEST_F(DerivationTest, Arrays) {
        // a network of two layers, the input a vector or a row, the gradients of the weights as finite differences give them
        exec("CREATE TABLE nn ( img double precision[], y double precision[], w1 double precision[], w2 double precision[] );");
        exec("INSERT INTO nn VALUES ('{0.5,-1,2}', '{0,1}', '{{0.1,0.2},{-0.3,0.4},{0.5,-0.6}}', '{{0.7,-0.8},{0.9,0.1}}'), "
             "('{{0.5,-1,2}}', '{{0,1}}', '{{0.1,0.2},{-0.3,0.4},{0.5,-0.6}}', '{{0.7,-0.8},{0.9,0.1}}');");
        const std::vector<std::pair<std::string, std::vector<double>>> losses = {
            {"(softmax(tanh(x.img ** x.w1) ** x.w2) - x.y)^2",
             {0.104505, 0.042672, -0.209009, -0.085343, 0.418018, 0.170687, 0.515992, -0.515992, -0.534349, 0.534349}},
            {"softmaxcce(sig(x.img ** x.w1) ** x.w2, x.y)",
             {0.097114, 0.047251, -0.194228, -0.094501, 0.388456, 0.189003, 0.628965, -0.628965, 0.144484, -0.144484}}};
        for (auto & [loss, expected] : losses) {
            rows_t rows = query("SELECT d_w1, d_w2 FROM derivation(TABLE (SELECT * FROM nn), lambda (x) (" + loss + "));");
            ASSERT_EQ(rows.size(), 2u);
            for (auto & row : rows) {
                std::vector<double> d;
                static const std::regex number(R"(-?[0-9][-0-9.e+]*)");
                for (std::sregex_iterator it(row.begin(), row.end(), number), end; it != end; ++it) d.push_back(std::stod(it->str()));
                ASSERT_EQ(d.size(), expected.size()) << row;
                for (size_t i = 0; i < d.size(); ++i) EXPECT_NEAR(d[i], expected[i], 1e-5) << loss << ": " << row;
            }
        }
        // a number that meets an array: the sum of its derivatives; an array of a number: the derivative repeated
        EXPECT_EQ(query("SELECT d_b FROM derivation(TABLE (SELECT img, w1, 0.5 AS b FROM nn), lambda (x) (relu(x.img ** x.w1 + x.b)));"),
                  (rows_t{"(1.5)", "(1.5)"}));
        EXPECT_EQ(query("SELECT d_w FROM derivation(TABLE (SELECT '{{1,2},{3,4}}'::float[] AS w), lambda (x) (x.w * 3));"), (rows_t{"({{3,3},{3,3}})"}));
        EXPECT_EQ(query("SELECT unbroadcast('{1,2,3}'::float[], 1), unbroadcast(2, '{1,2}'::float[]), unbroadcast(2, 1);"), (rows_t{"(6, {2,2}, 2)"}));
        // a column and a vector: the outer product
        EXPECT_EQ(query("SELECT '{{1},{2}}'::float[] ** '{3,4}'::float[];"), (rows_t{"({{3,4},{6,8}})"}));
    }

    TEST_F(DerivationTest, GradientDescent) {
        // least squares of y = 1 + 2 x, in one plan with the steps over the working table
        exec("CREATE TABLE samples ( x double precision, y double precision );");
        exec("INSERT INTO samples VALUES (0, 1), (1, 3), (2, 5), (3, 7);");
        const auto w = numbers("WITH RECURSIVE gd (id, a, b) AS (SELECT 0, 0::float, 0::float UNION ALL "
                               "SELECT id + 1, a - 0.05 * avg(d_a), b - 0.05 * avg(d_b) FROM derivation(TABLE (SELECT * FROM gd, samples WHERE id < 500), "
                               "lambda (x) ((x.a * x.x + x.b - x.y)^2)) GROUP BY id, a, b) SELECT a, b FROM gd WHERE id = 500;");
        ASSERT_EQ(w.size(), 2u);
        EXPECT_NEAR(w[0], 2.0, 1e-6);
        EXPECT_NEAR(w[1], 1.0, 1e-6);
    }

    TEST_F(DerivationTest, Functions) {
        EXPECT_EQ(query("SELECT exp(0), ln(exp(2)), log(1000), log(2, 8), sqrt(2.25), sin(0), cos(0), sqrt('{4,9}'::float[]), ln(NULL::float);"),
                  (rows_t{"(1, 2, 3, 3, 1.5, 0, 1, {2,3}, NULL)"}));
        EXPECT_THROW(exec("SELECT ln(0);"), std::exception);
        EXPECT_THROW(exec("SELECT log(-1);"), std::exception);
        EXPECT_THROW(exec("SELECT sqrt('{1,-1}'::float[]);"), std::exception);
        // no derivative, or not a lambda
        EXPECT_THROW(exec("SELECT * FROM derivation(TABLE (SELECT * FROM p), lambda (x) (highestposition(ARRAY[x.a])));"), std::exception);
        EXPECT_THROW(exec("SELECT * FROM derivation(TABLE (SELECT * FROM p), lambda (x) (x.a < 1));"), std::exception);
        EXPECT_THROW(exec("SELECT * FROM derivation(TABLE (SELECT * FROM p), x.a);"), std::exception);
        EXPECT_THROW(exec("SELECT * FROM derivation(TABLE (SELECT * FROM p), lambda (x) (x.a +));"), std::exception);
        EXPECT_EQ(query("SELECT count(*) FROM p;"), (rows_t{"(1)"}));
    }

} // namespace
