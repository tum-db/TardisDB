// ORDER BY: positions, names and aliases of the result, expressions (also of columns that are not selected,
// and aggregates), ASC and DESC with NULLS FIRST and LAST, each type, set operations, VALUES, WITH, errors

#include "codegen/LLVMCompat.hpp"
#include <mutex>
#include "foundations/Parallel.hpp"
#include "native/sql/SqlTuple.hpp"
#include "queryCompiler/queryCompiler.hpp"
#include "semanticAnalyser/SemanticAnalyser.hpp"
#include "sqlParser/SQLParser.hpp"
#include <gtest/gtest.h>

namespace {

    using rows_t = std::vector<std::string>;

    class OrderTest : public ::testing::Test {
    protected:
        void SetUp() override {
            db = std::make_unique<Database>();
            exec("CREATE TABLE o ( id integer not null, g integer, name varchar(10), t text, d double precision, "
                 "n numeric(6, 2), c char(4), day date, big bigint );");
            exec("INSERT INTO o VALUES (1, 2, 'b', 'xx', 1.5, 3.10, 'b', '2024-03-01', 30), "
                 "(2, 1, 'a', 'yy', NULL, 1.00, 'a  ', '2024-01-01', -5), "
                 "(3, NULL, 'c', 'aa', -2.0, 2.50, 'ab', '2023-12-31', NULL), "
                 "(4, 1, 'd', NULL, 0.5, NULL, NULL, NULL, 7);");
        }

        QueryCompiler::StatementResult exec(const std::string & sql) {
            return QueryCompiler::execute(sql, *db, "master", [](Native::Sql::SqlTuple *) { });
        }

        /// \returns The rows of the query in their order, the same with one thread and with parallel scans
        rows_t query(const std::string & sql) {
            auto run = [&]() {
                rows_t rows;
                std::mutex mutex;
                QueryCompiler::execute(sql, *db, "master", [&](Native::Sql::SqlTuple * tuple) {
                    std::lock_guard<std::mutex> lock(mutex);
                    rows.push_back(Native::Sql::toString(*tuple));
                });
                return rows;
            };
            const size_t threads = Parallel::threads, morselSize = Parallel::morselSize;
            auto rows = run();
            Parallel::threads = 4;
            Parallel::morselSize = 1;
            auto parallel = run();
            Parallel::threads = threads;
            Parallel::morselSize = morselSize;
            EXPECT_EQ(rows, parallel) << sql;
            return rows;
        }

        std::unique_ptr<Database> db;
    };

    TEST_F(OrderTest, Keys) {
        EXPECT_EQ(query("SELECT id FROM o ORDER BY id DESC;"), (rows_t{"(4)", "(3)", "(2)", "(1)"}));
        EXPECT_EQ(query("SELECT id, g FROM o ORDER BY 2, 1;"), (rows_t{"(2, 1)", "(4, 1)", "(1, 2)", "(3, NULL)"}));
        EXPECT_EQ(query("SELECT id AS k FROM o ORDER BY k DESC;"), (rows_t{"(4)", "(3)", "(2)", "(1)"}));
        EXPECT_EQ(query("SELECT id k FROM o ORDER BY k;"), (rows_t{"(1)", "(2)", "(3)", "(4)"}));
        // a column that is not selected, and expressions
        EXPECT_EQ(query("SELECT name FROM o ORDER BY id DESC;"), (rows_t{"(d)", "(c)", "(a)", "(b)"}));
        EXPECT_EQ(query("SELECT id FROM o ORDER BY id - 2 * (id / 2), id;"), (rows_t{"(2)", "(4)", "(1)", "(3)"}));
        EXPECT_EQ(query("SELECT id FROM o WHERE id > 1 ORDER BY 0 - id;"), (rows_t{"(4)", "(3)", "(2)"}));
        EXPECT_EQ(query("SELECT * FROM o ORDER BY name DESC;").front(), "(4, 1, d, NULL, 0.5, NULL, NULL, NULL, 7)");
        EXPECT_EQ(query("SELECT o.id FROM o ORDER BY o.name;"), (rows_t{"(2)", "(1)", "(3)", "(4)"}));
    }

    // as PostgreSQL: NULLs are greater than all values, unless NULLS FIRST or LAST says otherwise
    TEST_F(OrderTest, Nulls) {
        EXPECT_EQ(query("SELECT id FROM o ORDER BY g, id;"), (rows_t{"(2)", "(4)", "(1)", "(3)"}));
        EXPECT_EQ(query("SELECT id FROM o ORDER BY g DESC, id;"), (rows_t{"(3)", "(1)", "(2)", "(4)"}));
        EXPECT_EQ(query("SELECT id FROM o ORDER BY g NULLS FIRST, id DESC;"), (rows_t{"(3)", "(4)", "(2)", "(1)"}));
        EXPECT_EQ(query("SELECT id FROM o ORDER BY g DESC NULLS LAST, id;"), (rows_t{"(1)", "(2)", "(4)", "(3)"}));
    }

    TEST_F(OrderTest, Types) {
        EXPECT_EQ(query("SELECT id FROM o ORDER BY name;"), (rows_t{"(2)", "(1)", "(3)", "(4)"}));
        EXPECT_EQ(query("SELECT id FROM o ORDER BY t;"), (rows_t{"(3)", "(1)", "(2)", "(4)"}));
        EXPECT_EQ(query("SELECT id FROM o ORDER BY d;"), (rows_t{"(3)", "(4)", "(1)", "(2)"}));
        EXPECT_EQ(query("SELECT id FROM o ORDER BY n DESC NULLS LAST;"), (rows_t{"(1)", "(3)", "(2)", "(4)"}));
        EXPECT_EQ(query("SELECT id FROM o ORDER BY c;"), (rows_t{"(2)", "(3)", "(1)", "(4)"})); // 'a  ' as 'a'
        EXPECT_EQ(query("SELECT id FROM o ORDER BY day;"), (rows_t{"(3)", "(2)", "(1)", "(4)"}));
        EXPECT_EQ(query("SELECT id FROM o ORDER BY big;"), (rows_t{"(2)", "(4)", "(1)", "(3)"}));
        EXPECT_EQ(query("SELECT name FROM o ORDER BY name DESC;"), (rows_t{"(d)", "(c)", "(b)", "(a)"}));
    }

    TEST_F(OrderTest, Queries) {
        // aggregates: by a column of the result, its alias or an aggregate that is not selected
        EXPECT_EQ(query("SELECT g, count(*) FROM o GROUP BY g ORDER BY count(*) DESC, g;"), (rows_t{"(1, 2)", "(2, 1)", "(NULL, 1)"}));
        EXPECT_EQ(query("SELECT g, count(*) c FROM o GROUP BY g ORDER BY c, g NULLS FIRST;"), (rows_t{"(NULL, 1)", "(2, 1)", "(1, 2)"}));
        EXPECT_EQ(query("SELECT g FROM o GROUP BY g ORDER BY sum(id) DESC;"), (rows_t{"(1)", "(NULL)", "(2)"}));
        // set operations and VALUES: by position or name
        EXPECT_EQ(query("SELECT id FROM o WHERE id < 3 UNION SELECT g FROM o WHERE g IS NOT NULL ORDER BY 1 DESC;"), (rows_t{"(2)", "(1)"}));
        EXPECT_EQ(query("SELECT id FROM o UNION ALL SELECT g FROM o ORDER BY id NULLS FIRST;"),
                  (rows_t{"(NULL)", "(1)", "(1)", "(1)", "(2)", "(2)", "(3)", "(4)"}));
        EXPECT_EQ(query("VALUES (3), (1), (2) ORDER BY 1;"), (rows_t{"(1)", "(2)", "(3)"}));
        // WITH
        EXPECT_EQ(query("WITH RECURSIVE r(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM r WHERE i < 4) SELECT i FROM r ORDER BY i DESC;"),
                  (rows_t{"(4)", "(3)", "(2)", "(1)"}));
        EXPECT_EQ(query("WITH w AS (SELECT id, name FROM o WHERE g = 1) SELECT name FROM w ORDER BY id DESC;"), (rows_t{"(d)", "(a)"}));
        // joins
        exec("CREATE TABLE p ( id integer not null, w integer not null );");
        exec("INSERT INTO p VALUES (1, 9), (2, 8), (3, 7);");
        EXPECT_EQ(query("SELECT o.name, p.w FROM o, p WHERE o.id = p.id ORDER BY p.w;"), (rows_t{"(c, 7)", "(a, 8)", "(b, 9)"}));
        // the result describes the visible columns only
        auto result = exec("SELECT id FROM o ORDER BY name;");
        EXPECT_EQ(result.columnNames, (std::vector<std::string>{"id"}));
        EXPECT_EQ(result.rows, 4u);
        // EXPLAIN ANALYZE runs the query without ORDER BY
        EXPECT_EQ(exec("EXPLAIN ANALYZE SELECT id FROM o ORDER BY name;").rows, 1u);
    }

    TEST_F(OrderTest, Errors) {
        EXPECT_THROW(exec("SELECT id FROM o ORDER BY 0;"), semanticalAnalysis::semantic_sql_error);
        EXPECT_THROW(exec("SELECT id FROM o ORDER BY 2;"), semanticalAnalysis::semantic_sql_error);
        EXPECT_THROW(exec("SELECT id FROM o ORDER BY nosuch;"), std::exception);
        EXPECT_THROW(exec("SELECT id FROM o ORDER BY;"), std::exception);
        EXPECT_THROW(exec("SELECT id FROM o UNION SELECT g FROM o ORDER BY id + 1;"), semanticalAnalysis::semantic_sql_error);
        EXPECT_THROW(exec("SELECT * FROM o ORDER BY id + 1;"), semanticalAnalysis::semantic_sql_error);
        EXPECT_THROW(exec("SELECT id FROM o ORDER BY id ORDER BY id;"), std::exception);
        exec("CREATE TABLE ar ( a integer[] );");
        EXPECT_THROW(exec("SELECT a FROM ar ORDER BY a;"), semanticalAnalysis::semantic_sql_error); // also without rows
    }
}
