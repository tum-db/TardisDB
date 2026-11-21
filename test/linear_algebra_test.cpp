// Linear algebra on arrays of numbers, without BLAS: + - * / ^ element by
// element (also with a number), ** (matrix and tensor products), @ and abs, transpose, kroneckerproduct,
// sig, relu, relup, tanh, softmax, softmaxcce, softmaxccep and highestposition; NULLs, errors, a product
// larger than the blocks of the kernel, and gradient descent in a recursive common table expression

#include "codegen/LLVMCompat.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <regex>
#include <sstream>

#include "foundations/Parallel.hpp"
#include "native/sql/SqlTuple.hpp"
#include "queryCompiler/queryCompiler.hpp"
#include <gtest/gtest.h>

namespace {

    class LinearAlgebraTest : public ::testing::Test {
    protected:
        void SetUp() override {
            db = std::make_unique<Database>();
            exec("CREATE TABLE m ( id integer not null, a double precision[], b integer[], v numeric(6,2)[], s text[] );");
            exec("INSERT INTO m VALUES (1, '{{1,2},{3,4}}', '{{5,6},{7,8}}', '{1.5,-2.5}', '{{x,y},{z,w}}'), "
                 "(2, '{{1,NULL},{3,4}}', '{{1,0},{0,1}}', '{NULL,1}', NULL), (3, NULL, NULL, NULL, NULL);");
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

        /// \returns The single value of a query (of the row of id 1, if the query reads m)
        std::string value(const std::string & sql) {
            auto rows = query(sql);
            if (rows.size() != 1) return "<" + std::to_string(rows.size()) + " rows>";
            return rows.front().substr(1, rows.front().size() - 2);
        }

        using rows_t = std::vector<std::string>;
        std::unique_ptr<Database> db;
    };

    TEST_F(LinearAlgebraTest, ElementwiseOperators) {
        EXPECT_EQ(value("SELECT a + b, a - b, a * b FROM m WHERE id = 1;"), "{{6,8},{10,12}}, {{-4,-4},{-4,-4}}, {{5,12},{21,32}}");
        EXPECT_EQ(value("SELECT a / b FROM m WHERE id = 1;"), "{{0.2,0.3333333333333333},{0.42857142857142855,0.5}}");
        // with a number on either side, ^ of each element, unary minus
        EXPECT_EQ(value("SELECT 2 * a, a * 0.5, 1 - a, a - 1, a ^ 2, 2 ^ b, - a FROM m WHERE id = 1;"),
                  "{{2,4},{6,8}}, {{0.5,1},{1.5,2}}, {{0,-1},{-2,-3}}, {{0,1},{2,3}}, {{1,4},{9,16}}, {{32,64},{128,256}}, {{-1,-2},{-3,-4}}");
        EXPECT_EQ(value("SELECT v * 2, v + v FROM m WHERE id = 1;"), "{3,-5}, {3,-5}");
        // ^ binds closer than * and unary minus closer than ^, as in PostgreSQL
        EXPECT_EQ(value("SELECT 2 * a ^ 2, - 2 ^ 2 FROM m WHERE id = 1;"), "{{2,8},{18,32}}, 4");
        // a NULL element gives NULL elements, a NULL array NULL
        EXPECT_EQ(value("SELECT a + 1, a * b FROM m WHERE id = 2;"), "{{2,NULL},{4,5}}, {{1,NULL},{0,4}}");
        EXPECT_EQ(value("SELECT a + 1 FROM m WHERE id = 3;"), "NULL");
        EXPECT_THROW(exec("SELECT a + '{1,2}' FROM m;"), std::exception);       // different shapes
        EXPECT_THROW(exec("SELECT a / 0 FROM m WHERE id = 1;"), std::exception); // division by zero
        EXPECT_THROW(exec("SELECT s + 1 FROM m;"), std::exception);              // not numbers
    }

    TEST_F(LinearAlgebraTest, Products) {
        EXPECT_EQ(value("SELECT a ** b FROM m WHERE id = 1;"), "{{19,22},{43,50}}");
        EXPECT_EQ(value("SELECT a ** b ** b, a ** (b ** b) FROM m WHERE id = 1;"), "{{249,290},{565,658}}, {{249,290},{565,658}}");
        // like the other operators of PostgreSQL that are not arithmetic, ** binds less closely than + - * /
        EXPECT_EQ(value("SELECT a ** b - b, (a ** b) - b FROM m WHERE id = 1;"), "{{0,0},{0,0}}, {{14,16},{36,42}}");
        // vectors: a matrix times a vector, and the dot product
        EXPECT_EQ(value("SELECT a ** v, v ** a, v ** v FROM m WHERE id = 1;"), "{-3.5,-5.5}, {-6,-7}, {8.5}");
        EXPECT_EQ(value("SELECT a ** '{{1,0,0},{0,1,0}}' FROM m WHERE id = 1;"), "{{1,2,0},{3,4,0}}");
        // tensors: the last dimension of the first, the first of the second
        EXPECT_EQ(value("SELECT '{{{1,2},{3,4}}}'::double precision[] ** '{1,1}' FROM m WHERE id = 1;"), "{{3,7}}");
        EXPECT_EQ(value("SELECT kroneckerproduct(a, '{{0,1},{1,0}}') FROM m WHERE id = 1;"), "{{0,1,0,2},{1,0,2,0},{0,3,0,4},{3,0,4,0}}");
        // a NULL makes the sums over its row (of the first) or column (of the second) NULL
        EXPECT_EQ(value("SELECT a ** b FROM m WHERE id = 2;"), "{{NULL,NULL},{3,4}}");
        EXPECT_EQ(value("SELECT b ** a FROM m WHERE id = 2;"), "{{1,NULL},{3,NULL}}");
        EXPECT_THROW(exec("SELECT a ** '{{1,2,3}}' FROM m WHERE id = 1;"), std::exception);
        EXPECT_THROW(exec("SELECT a ** 2 FROM m WHERE id = 1;"), std::exception);
        EXPECT_THROW(exec("SELECT kroneckerproduct(a, v) FROM m WHERE id = 1;"), std::exception);
    }

    TEST_F(LinearAlgebraTest, TransposeAndFunctions) {
        EXPECT_EQ(value("SELECT transpose(a), transpose(v), transpose(s) FROM m WHERE id = 1;"), "{{1,3},{2,4}}, {{1.50},{-2.50}}, {{x,z},{y,w}}");
        EXPECT_EQ(value("SELECT transpose('{{{1,2},{3,4}},{{5,6},{7,8}}}'::integer[]) FROM m WHERE id = 1;"), "{{{1,2},{5,6}},{{3,4},{7,8}}}");
        EXPECT_EQ(value("SELECT @ v, abs(v), relu(v), relup(v) FROM m WHERE id = 1;"), "{1.5,2.5}, {1.5,2.5}, {1.5,0}, {1,0}");
        EXPECT_EQ(value("SELECT sig(a) FROM m WHERE id = 1;"),
                  "{{0.7310585786300049,0.8807970779778823},{0.9525741268224334,0.9820137900379085}}");
        EXPECT_EQ(value("SELECT tanh(v) FROM m WHERE id = 1;"), "{0.9051482536448664,-0.9866142981514303}");
        // of numbers
        EXPECT_EQ(value("SELECT sig(0), relu(-1.5), relup(2), @ -3, abs(-2.5), tanh(0), 2 ^ 10;"), "0.5, 0, 1, 3, 2.5, 0, 1024");
        EXPECT_EQ(value("SELECT sig(a), @ v FROM m WHERE id = 2;"), "{{0.7310585786300049,NULL},{0.9525741268224334,0.9820137900379085}}, {NULL,1}");
    }

    TEST_F(LinearAlgebraTest, SoftmaxAndPositions) {
        EXPECT_EQ(value("SELECT softmax(v), highestposition(b), highestposition(v) FROM m WHERE id = 1;"),
                  "{0.9820137900379085,0.017986209962091555}, 3, 0");
        // the cross-entropy of softmax and the labels, and its gradient softmax - labels
        EXPECT_EQ(value("SELECT softmaxcce(v, '{1,0}'), softmaxccep(v, '{1,0}') FROM m WHERE id = 1;"),
                  "0.01814992791780978, {-0.01798620996209155,0.017986209962091555}");
        // large values do not overflow (the exponentials are shifted by the maximum)
        EXPECT_EQ(value("SELECT softmax('{1000,1000}'), highestposition('{1,5,5}');"), "{0.5,0.5}, 1");
        // over all elements: NULL if one is NULL
        EXPECT_EQ(value("SELECT softmax(v), highestposition(v), softmaxcce(v, '{1,0}') FROM m WHERE id = 2;"), "NULL, NULL, NULL");
        EXPECT_THROW(exec("SELECT softmaxcce(v, '{1,0,0}') FROM m WHERE id = 1;"), std::exception);
        EXPECT_THROW(exec("SELECT softmax(1);"), std::exception);
    }

    TEST_F(LinearAlgebraTest, LargeProduct) {
        // larger than the blocks of the kernel (64 x 256 x 512), against the sums of a plain loop
        const size_t m = 70, k = 300, n = 530;
        auto matrix = [](size_t rows, size_t columns, int seed) {
            std::vector<long> values(rows * columns);
            for (size_t i = 0; i < values.size(); ++i) values[i] = static_cast<long>((i * 7919 + seed * 104729) % 23) - 11;
            return values;
        };
        auto text = [](const std::vector<long> & values, size_t rows, size_t columns) {
            std::ostringstream out;
            out << "{";
            for (size_t i = 0; i < rows; ++i) {
                out << (i ? ",{" : "{");
                for (size_t j = 0; j < columns; ++j) out << (j ? "," : "") << values[i * columns + j];
                out << "}";
            }
            out << "}";
            return out.str();
        };
        auto x = matrix(m, k, 1), y = matrix(k, n, 2);
        std::vector<long> expected(m * n, 0);
        for (size_t i = 0; i < m; ++i)
            for (size_t j = 0; j < n; ++j)
                for (size_t l = 0; l < k; ++l) expected[i * n + j] += x[i * k + l] * y[l * n + j];
        exec("CREATE TABLE big ( x integer[], y integer[] );");
        exec("INSERT INTO big VALUES ('" + text(x, m, k) + "', '" + text(y, k, n) + "');");
        EXPECT_EQ(value("SELECT x ** y FROM big;"), text(expected, m, n));
    }

    TEST_F(LinearAlgebraTest, GradientDescent) {
        // least squares of y = 1 + 2 x in a recursive common table expression: w - 0.2 * X^T (X w - y) / n
        exec("CREATE TABLE data ( x double precision[], y double precision[] );");
        exec("INSERT INTO data VALUES ('{{1,0},{1,1},{1,2},{1,3}}', '{{1},{3},{5},{7}}');");
        auto rows = query("WITH RECURSIVE gd(i, w) AS (SELECT 0, '{{0},{0}}'::double precision[] "
                          "UNION ALL SELECT gd.i + 1, gd.w - 0.2 * (transpose(data.x) ** ((data.x ** gd.w) - data.y)) / 4 "
                          "FROM gd, data WHERE gd.i < 500) SELECT w FROM gd WHERE i = 500;");
        ASSERT_EQ(rows.size(), 1u);
        std::regex number(R"([-0-9.e+]+)");
        std::vector<double> w;
        for (std::sregex_iterator it(rows[0].begin(), rows[0].end(), number), end; it != end; ++it) w.push_back(std::stod(it->str()));
        ASSERT_EQ(w.size(), 2u) << rows[0];
        EXPECT_NEAR(w[0], 1.0, 1e-6);
        EXPECT_NEAR(w[1], 2.0, 1e-6);
    }

}
