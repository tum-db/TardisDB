// WITH [ RECURSIVE ], UNION, INTERSECT and EXCEPT [ ALL ], VALUES and SELECT without FROM: common table
// expressions computed into temporary tables, recursion over hierarchies and graphs (with and without
// duplicates), set operations on multisets, the columns of the result, branches and transactions, NULLs
// and arrays, and the errors of PostgreSQL

#include "codegen/LLVMCompat.hpp"

#include <algorithm>
#include <mutex>

#include "foundations/Parallel.hpp"
#include "runtime/SqlTuple.hpp"
#include "queryCompiler/queryCompiler.hpp"
#include <gtest/gtest.h>

namespace {

    class CteTest : public ::testing::Test {
    protected:
        void SetUp() override {
            db = std::make_unique<Database>();
            exec("CREATE TABLE emp ( id integer not null, boss integer, name varchar(10) not null );");
            exec("INSERT INTO emp VALUES (1, NULL, 'ceo'), (2, 1, 'cto'), (3, 1, 'cfo'), (4, 2, 'dev'), (5, 4, 'intern'), "
                 "(6, NULL, 'other'), (7, 2, 'ops');");
            exec("CREATE TABLE edge ( a integer not null, b integer not null );");
            exec("INSERT INTO edge VALUES (1, 2), (2, 3), (3, 1), (3, 4), (5, 6);");
        }

        QueryCompiler::StatementResult exec(const std::string & sql, const std::string & branch = "master",
                                            const QueryCompiler::Transaction * transaction = nullptr) {
            return QueryCompiler::execute(sql, *db, branch, [](Native::Sql::SqlTuple *) { }, nullptr, transaction);
        }

        /// \returns The rows of the query, sorted, the same with one thread and with parallel scans
        std::vector<std::string> query(const std::string & sql, const std::string & branch = "master",
                                       const QueryCompiler::Transaction * transaction = nullptr) {
            auto run = [&]() {
                std::vector<std::string> rows;
                std::mutex mutex;
                QueryCompiler::execute(sql, *db, branch, [&](Native::Sql::SqlTuple * tuple) {
                    std::lock_guard<std::mutex> lock(mutex);
                    rows.push_back(Native::Sql::toString(*tuple));
                }, nullptr, transaction);
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

    TEST_F(CteTest, SelectWithoutFrom) {
        EXPECT_EQ(query("SELECT 1;"), (rows_t{"(1)"}));
        EXPECT_EQ(query("SELECT 1 + 2 AS three, 'x';"), (rows_t{"(3, x)"}));
        auto described = exec("SELECT 1 + 2 AS three, 4;");
        EXPECT_EQ(described.columnNames, (std::vector<std::string>{"three", "?column?"}));
        EXPECT_EQ(described.rows, 1u);
        EXPECT_THROW(exec("SELECT nope;"), std::exception);
    }

    TEST_F(CteTest, RecursiveCount) {
        auto result = exec("WITH RECURSIVE t(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM t WHERE n < 5) SELECT n FROM t;");
        EXPECT_EQ(result.tag, "SELECT");
        EXPECT_EQ(result.rows, 5u);
        EXPECT_EQ(result.columnNames, (std::vector<std::string>{"n"}));
        EXPECT_EQ(query("with recursive t ( n ) as ( select 1 union all select n + 1 from t where n < 5 ) select n , n * n from t;"),
                  (rows_t{"(1, 1)", "(2, 4)", "(3, 9)", "(4, 16)", "(5, 25)"}));
        // aggregates over a common table expression, and an empty anchor
        EXPECT_EQ(query("WITH RECURSIVE t(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM t WHERE n < 100) SELECT count(*), sum(n) FROM t;"),
                  (rows_t{"(100, 5050)"}));
        EXPECT_TRUE(query("WITH RECURSIVE t(n) AS (SELECT id FROM emp WHERE id > 99 UNION ALL SELECT n + 1 FROM t) SELECT n FROM t;").empty());
    }

    TEST_F(CteTest, Hierarchies) {
        // the subordinates of the cto, with their depth
        EXPECT_EQ(query("WITH RECURSIVE sub(id, name, depth) AS ("
                        " SELECT id, name, 0 FROM emp WHERE id = 2"
                        " UNION ALL SELECT e.id, e.name, s.depth + 1 FROM emp e, sub s WHERE e.boss = s.id"
                        ") SELECT name, depth FROM sub;"),
                  (rows_t{"(cto, 0)", "(dev, 1)", "(intern, 2)", "(ops, 1)"}));
        // the chain of bosses of the intern, the recursive table on the left of the join
        EXPECT_EQ(query("WITH RECURSIVE up(id, boss, level) AS ("
                        " SELECT id, boss, 0 FROM emp WHERE name = 'intern'"
                        " UNION ALL SELECT e.id, e.boss, u.level + 1 FROM up u, emp e WHERE u.boss = e.id"
                        ") SELECT emp.name, up.level FROM up, emp WHERE up.id = emp.id;"),
                  (rows_t{"(ceo, 3)", "(cto, 2)", "(dev, 1)", "(intern, 0)"}));
    }

    TEST_F(CteTest, GraphsWithCycles) {
        // UNION removes duplicates, so the recursion ends on the cycle 1 -> 2 -> 3 -> 1
        EXPECT_EQ(query("WITH RECURSIVE reach(node) AS (SELECT 1 UNION SELECT edge.b FROM reach, edge WHERE reach.node = edge.a) SELECT node FROM reach;"),
                  (rows_t{"(1)", "(2)", "(3)", "(4)"}));
        // UNION ALL with a bound on the length of the paths keeps every path
        EXPECT_EQ(query("WITH RECURSIVE path(node, len) AS (SELECT 1, 0 UNION ALL SELECT e.b, p.len + 1 FROM path p, edge e "
                        "WHERE p.node = e.a AND p.len < 4) SELECT len, node FROM path;"),
                  (rows_t{"(0, 1)", "(1, 2)", "(2, 3)", "(3, 1)", "(3, 4)", "(4, 2)"}));
        // the anchor of UNION has no duplicates either
        EXPECT_EQ(query("WITH RECURSIVE r(n) AS (VALUES (1), (1), (2) UNION SELECT n FROM r) SELECT n FROM r;"), (rows_t{"(1)", "(2)"}));
    }

    TEST_F(CteTest, CommonTableExpressions) {
        // several, each reading the ones before; a column list; one read twice
        EXPECT_EQ(query("WITH low AS (SELECT id, name FROM emp WHERE id < 5), mid(i) AS (SELECT id FROM low WHERE id > 1) "
                        "SELECT low.name FROM low, mid WHERE low.id = mid.i;"), (rows_t{"(cfo)", "(cto)", "(dev)"}));
        EXPECT_EQ(query("WITH c AS (SELECT id, boss FROM emp WHERE boss IS NOT NULL) SELECT x.id, y.id FROM c x, c y WHERE x.boss = y.id;"),
                  (rows_t{"(4, 2)", "(5, 4)", "(7, 2)"}));
        // a common table expression hides a table of its name; without RECURSIVE, its own query reads the table
        EXPECT_EQ(query("WITH emp AS (SELECT 42 AS id) SELECT id FROM emp;"), (rows_t{"(42)"}));
        EXPECT_EQ(query("WITH emp AS (SELECT count(*) AS n FROM emp) SELECT n FROM emp;"), (rows_t{"(7)"}));
        // NULLs, arrays and doubles in the rows
        EXPECT_EQ(query("WITH b AS (SELECT name, boss FROM emp WHERE boss IS NULL) SELECT name, boss FROM b;"),
                  (rows_t{"(ceo, NULL)", "(other, NULL)"}));
        EXPECT_EQ(query("WITH RECURSIVE a(v) AS (SELECT ARRAY[1] UNION ALL SELECT array_append(v, cardinality(v) + 1) FROM a "
                        "WHERE cardinality(v) < 3) SELECT v FROM a;"),
                  (rows_t{"({1,2,3})", "({1,2})", "({1})"}));
        EXPECT_EQ(query("WITH RECURSIVE h(x) AS (SELECT 1.0::double precision UNION ALL SELECT x / 2 FROM h WHERE x > 0.2) SELECT x FROM h;"),
                  (rows_t{"(0.125)", "(0.25)", "(0.5)", "(1)"}));
    }

    TEST_F(CteTest, UnionAndValues) {
        EXPECT_EQ(query("SELECT id FROM emp WHERE id < 3 UNION SELECT boss FROM emp WHERE boss = 1;"), (rows_t{"(1)", "(2)"}));
        EXPECT_EQ(query("SELECT id FROM emp WHERE id < 3 UNION ALL SELECT boss FROM emp WHERE boss = 1;"),
                  (rows_t{"(1)", "(1)", "(1)", "(2)"}));
        // from left to right: the UNION removes the duplicates of the UNION ALL before it
        EXPECT_EQ(query("SELECT 1 UNION ALL SELECT 1 UNION SELECT 2;"), (rows_t{"(1)", "(2)"}));
        EXPECT_EQ(query("SELECT 1 UNION SELECT 1 UNION ALL SELECT 1;"), (rows_t{"(1)", "(1)"}));
        EXPECT_EQ(query("(SELECT 1) UNION (SELECT 2);"), (rows_t{"(1)", "(2)"}));
        auto values = exec("VALUES (1, 'a'), (2, 'b');");
        EXPECT_EQ(values.columnNames, (std::vector<std::string>{"column1", "column2"}));
        EXPECT_EQ(values.rows, 2u);
        EXPECT_EQ(query("VALUES (1, 'a'), (1, 'a');"), (rows_t{"(1, a)", "(1, a)"}));
        // the first query names and types the columns; strings of other lengths fit
        auto united = exec("SELECT name AS who FROM emp WHERE id = 1 UNION SELECT 'a much longer name';");
        EXPECT_EQ(united.columnNames, (std::vector<std::string>{"who"}));
        EXPECT_EQ(united.columnTypes[0].typeID, Sql::SqlType::TypeID::TextID);
        EXPECT_EQ(query("SELECT name FROM emp WHERE id = 1 UNION SELECT 'a much longer name';"), (rows_t{"(a much longer name)", "(ceo)"}));
        // NULLs are no duplicates of values, but of each other
        EXPECT_EQ(query("SELECT boss FROM emp UNION SELECT boss FROM emp WHERE id = 6;"), (rows_t{"(1)", "(2)", "(4)", "(NULL)"}));
        EXPECT_THROW(exec("SELECT 1 UNION SELECT 1, 2;"), std::exception);
        // the columns take the types of all queries, as in PostgreSQL
        auto numbers = exec("SELECT 1 UNION SELECT 2.5;");
        EXPECT_EQ(numbers.columnTypes[0].typeID, Sql::SqlType::TypeID::NumericID);
        EXPECT_EQ(query("SELECT 1 UNION SELECT 2.5 UNION SELECT 1.0;"), (rows_t{"(1.0)", "(2.5)"}));
        EXPECT_EQ(query("SELECT 1 UNION ALL SELECT 2.5::double precision;"), (rows_t{"(1)", "(2.5)"}));
        EXPECT_THROW(exec("SELECT 1 UNION SELECT 'a';"), std::exception);
        // a recursive query keeps the types of the others
        EXPECT_THROW(exec("WITH RECURSIVE t(n) AS (SELECT 1 UNION ALL SELECT n + 0.5 FROM t WHERE n < 3) SELECT n FROM t;"), std::exception);
    }

    TEST_F(CteTest, IntersectAndExcept) {
        exec("CREATE TABLE a ( x integer );");
        exec("CREATE TABLE b ( x integer );");
        exec("INSERT INTO a VALUES (1), (1), (1), (2), (3), (NULL), (NULL);");
        exec("INSERT INTO b VALUES (1), (1), (3), (4), (NULL);");
        // without ALL each row once; with ALL the lesser count, or the difference of the counts
        EXPECT_EQ(query("SELECT x FROM a INTERSECT SELECT x FROM b;"), (rows_t{"(1)", "(3)", "(NULL)"}));
        EXPECT_EQ(query("SELECT x FROM a INTERSECT ALL SELECT x FROM b;"), (rows_t{"(1)", "(1)", "(3)", "(NULL)"}));
        EXPECT_EQ(query("SELECT x FROM a INTERSECT DISTINCT SELECT x FROM b;"), (rows_t{"(1)", "(3)", "(NULL)"}));
        EXPECT_EQ(query("SELECT x FROM a EXCEPT SELECT x FROM b;"), (rows_t{"(2)"}));
        EXPECT_EQ(query("SELECT x FROM a EXCEPT ALL SELECT x FROM b;"), (rows_t{"(1)", "(2)", "(NULL)"}));
        EXPECT_EQ(query("SELECT x FROM b EXCEPT ALL SELECT x FROM a;"), (rows_t{"(4)"}));
        // INTERSECT binds closer than UNION and EXCEPT, which combine from left to right
        EXPECT_EQ(query("SELECT x FROM a UNION SELECT x FROM b INTERSECT SELECT x FROM a;"), (rows_t{"(1)", "(2)", "(3)", "(NULL)"}));
        EXPECT_EQ(query("SELECT x FROM b EXCEPT SELECT x FROM a UNION ALL SELECT x FROM a WHERE x = 1;"),
                  (rows_t{"(1)", "(1)", "(1)", "(4)"}));
        EXPECT_EQ(query("SELECT x FROM a EXCEPT (SELECT x FROM b EXCEPT SELECT 1);"), (rows_t{"(1)", "(2)"}));
        EXPECT_EQ(query("(SELECT x FROM a UNION SELECT x FROM b) INTERSECT VALUES (4), (2);"), (rows_t{"(2)", "(4)"}));
        // in the common type of all queries; in a common table expression and its recursion
        auto numbers = exec("SELECT 1.5 EXCEPT SELECT 1;");
        EXPECT_EQ(numbers.columnTypes[0].typeID, Sql::SqlType::TypeID::NumericID);
        EXPECT_EQ(numbers.rows, 1u);
        EXPECT_EQ(query("WITH c AS (SELECT x FROM a INTERSECT ALL SELECT x FROM b) SELECT count(*), count(x) FROM c;"), (rows_t{"(4, 3)"}));
        EXPECT_EQ(query("WITH RECURSIVE t(n) AS (SELECT x FROM a EXCEPT SELECT x FROM b UNION ALL SELECT n + 1 FROM t WHERE n < 4) "
                        "SELECT n FROM t;"), (rows_t{"(2)", "(3)", "(4)"}));
        // the recursive query is the right side of the outermost UNION, as in PostgreSQL
        EXPECT_THROW(exec("WITH RECURSIVE t(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM t WHERE n < 3 EXCEPT SELECT 2) SELECT n FROM t;"),
                     std::exception);
        EXPECT_THROW(exec("WITH RECURSIVE t(n) AS (SELECT 1 UNION ALL (SELECT n + 1 FROM t INTERSECT SELECT 2)) SELECT n FROM t;"),
                     std::exception);
        EXPECT_THROW(exec("SELECT x FROM a INTERSECT SELECT x, x FROM b;"), std::exception);
        EXPECT_THROW(exec("SELECT x FROM a EXCEPT;"), std::exception);
    }

    TEST_F(CteTest, BranchesAndTransactions) {
        exec("CREATE BRANCH dev FROM master;");
        exec("INSERT INTO emp VERSION dev VALUES (8, 5, 'trainee');");
        const std::string sub = "WITH RECURSIVE sub(id) AS (SELECT 4 UNION ALL SELECT e.id FROM emp e, sub s WHERE e.boss = s.id) "
                                "SELECT count(*) FROM sub;";
        EXPECT_EQ(query(sub), (rows_t{"(2)"}));
        EXPECT_EQ(query(sub, "dev"), (rows_t{"(3)"})); // the session's branch
        EXPECT_EQ(query("WITH RECURSIVE sub(id) AS (SELECT 4 UNION ALL SELECT e.id FROM emp VERSION dev e, sub s WHERE e.boss = s.id) "
                        "SELECT id FROM sub;"), (rows_t{"(4)", "(5)", "(8)"}));
        // a common table expression has no versions
        EXPECT_THROW(exec("WITH c AS (SELECT 1 AS x) SELECT x FROM c VERSION dev;"), std::exception);
        // in a transaction, its own writes
        auto transaction = QueryCompiler::beginTransaction(*db, "master");
        exec("INSERT INTO emp VALUES (9, 5, 'temp');", "master", &transaction);
        EXPECT_EQ(query(sub, "master", &transaction), (rows_t{"(3)"}));
        EXPECT_EQ(query(sub), (rows_t{"(2)"}));
        EXPECT_EQ(query("SELECT 1 UNION SELECT 2;", "master", &transaction), (rows_t{"(1)", "(2)"}));
        QueryCompiler::rollbackTransaction(*db, transaction);
    }

    TEST_F(CteTest, Errors) {
        // the forms of PostgreSQL
        EXPECT_THROW(exec("WITH RECURSIVE t(n) AS (SELECT n + 1 FROM t) SELECT n FROM t;"), std::exception);
        EXPECT_THROW(exec("WITH RECURSIVE t(n) AS (SELECT 1 UNION ALL SELECT t.n + 1 FROM t, t t2) SELECT n FROM t;"), std::exception);
        EXPECT_THROW(exec("WITH RECURSIVE t(n) AS (SELECT n FROM t UNION ALL SELECT 1) SELECT n FROM t;"), std::exception);
        EXPECT_THROW(exec("WITH t(a, b) AS (SELECT 1) SELECT a FROM t;"), std::exception);
        EXPECT_THROW(exec("WITH t AS (SELECT 1 AS x), t AS (SELECT 2 AS x) SELECT x FROM t;"), std::exception);
        EXPECT_THROW(exec("WITH t AS (SELECT 1, 2) SELECT 1 FROM t;"), std::exception); // two columns named ?column?
        EXPECT_THROW(exec("WITH t AS (INSERT INTO emp VALUES (10, 1, 'x')) SELECT 1;"), std::exception);
        EXPECT_THROW(exec("WITH t AS (SELECT 1 AS x SELECT x FROM t;"), std::exception);
        // nothing was written, and the database still answers
        EXPECT_EQ(query("SELECT count(*) FROM emp;"), (rows_t{"(7)"}));
    }

}
