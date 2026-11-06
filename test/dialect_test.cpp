// The SQL that PostgreSQL clients send: range and row-value predicates, IN, BETWEEN, <>, string
// comparisons, multi-row INSERT, PRIMARY KEY, the type names of PostgreSQL, nullable declarations,
// literals with spaces and casts, and the statements of a session (QueryCompiler::execute)

#include "codegen/LLVMCompat.hpp"

#include <algorithm>
#include <mutex>

#include "foundations/Parallel.hpp"
#include "native/sql/SqlTuple.hpp"
#include "queryCompiler/queryCompiler.hpp"
#include <gtest/gtest.h>

namespace {

    class DialectTest : public ::testing::Test {
    protected:
        void SetUp() override {
            db = std::make_unique<Database>();
        }

        QueryCompiler::StatementResult exec(const std::string & sql, const std::string & branch = "master") {
            return QueryCompiler::execute(sql, *db, branch, [](Native::Sql::SqlTuple *) { });
        }

        /// \returns The rows of the query in \p branch, sorted, the same with one thread and with parallel scans
        std::vector<std::string> query(const std::string & sql, const std::string & branch = "master") {
            auto run = [&]() {
                std::vector<std::string> rows;
                std::mutex mutex;
                QueryCompiler::execute(sql, *db, branch, [&](Native::Sql::SqlTuple * tuple) {
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

        void createKeys() {
            exec("CREATE TABLE p ( w integer not null, d integer not null, c integer not null, v varchar(5) not null, "
                 "f char(2) not null, PRIMARY KEY (w, d, c) );");
            exec("INSERT INTO p VALUES (1, 1, 1, 'a', 'aa'), (1, 1, 2, 'b', 'bb'), (1, 2, 1, 'c', 'cc'), "
                 "(2, 1, 1, 'dd', 'dd'), (2, 2, 5, 'e', 'ee');");
        }

        std::unique_ptr<Database> db;
    };

    TEST_F(DialectTest, RangesOfOneColumn) {
        createKeys();
        EXPECT_EQ(query("SELECT v FROM p WHERE c >= 2;"), (std::vector<std::string>{"(b)", "(e)"}));
        EXPECT_EQ(query("SELECT v FROM p WHERE c<2 AND w = 1;"), (std::vector<std::string>{"(a)", "(c)"}));
        EXPECT_EQ(query("SELECT v FROM p WHERE 2 < c;"), (std::vector<std::string>{"(e)"}));
        EXPECT_EQ(query("SELECT v FROM p WHERE c BETWEEN 2 AND 5;"), (std::vector<std::string>{"(b)", "(e)"}));
        EXPECT_EQ(query("SELECT v FROM p WHERE c != 2 AND c < 5;"), (std::vector<std::string>{"(a)", "(c)", "(dd)"}));
        EXPECT_EQ(query("SELECT v FROM p WHERE c <> 1;"), (std::vector<std::string>{"(b)", "(e)"}));
        EXPECT_EQ(query("SELECT v FROM p WHERE c IN (2, 5);"), (std::vector<std::string>{"(b)", "(e)"}));
    }

    TEST_F(DialectTest, RowValuesCompareLexicographically) {
        createKeys();
        EXPECT_EQ(query("SELECT v FROM p WHERE (w, d, c) >= (1, 1, 2) AND (w, d, c) <= (2, 1, 1);"),
                  (std::vector<std::string>{"(b)", "(c)", "(dd)"}));
        EXPECT_EQ(query("SELECT v FROM p WHERE (w, d, c) > (1, 2, 1);"), (std::vector<std::string>{"(dd)", "(e)"}));
        EXPECT_EQ(query("SELECT v FROM p WHERE (w, d, c) < (1, 2, 1);"), (std::vector<std::string>{"(a)", "(b)"}));
        EXPECT_EQ(query("SELECT v FROM p WHERE (w, d) = (1, 1);"), (std::vector<std::string>{"(a)", "(b)"}));
        EXPECT_EQ(query("SELECT v FROM p WHERE (w, d, c) <> (1, 1, 1) AND w = 1;"), (std::vector<std::string>{"(b)", "(c)"}));
    }

    TEST_F(DialectTest, StringsCompare) {
        createKeys();
        EXPECT_EQ(query("SELECT v FROM p WHERE v = 'dd';"), (std::vector<std::string>{"(dd)"}));
        EXPECT_EQ(query("SELECT v FROM p WHERE v > 'c';"), (std::vector<std::string>{"(dd)", "(e)"}));
        EXPECT_EQ(query("SELECT v FROM p WHERE v <= 'b';"), (std::vector<std::string>{"(a)", "(b)"}));
        EXPECT_EQ(query("SELECT v FROM p WHERE v IN ('a', 'e');"), (std::vector<std::string>{"(a)", "(e)"}));
        EXPECT_EQ(query("SELECT f FROM p WHERE f < 'cc';"), (std::vector<std::string>{"(aa)", "(bb)"}));
        // a common prefix: the shorter string first
        exec("INSERT INTO p VALUES (3, 1, 1, 'd', 'd');");
        EXPECT_EQ(query("SELECT v FROM p WHERE v < 'dd' AND v > 'c';"), (std::vector<std::string>{"(d)"}));
    }

    TEST_F(DialectTest, PredicatesOfUpdateAndDelete) {
        createKeys();
        EXPECT_EQ(exec("UPDATE p SET v = 'x' WHERE (w, d, c) >= (2, 1, 1);").rows, 2u);
        EXPECT_EQ(query("SELECT w, d, c FROM p WHERE v = 'x';"), (std::vector<std::string>{"(2, 1, 1)", "(2, 2, 5)"}));
        EXPECT_EQ(exec("DELETE FROM p WHERE c > 1;").rows, 2u);
        EXPECT_EQ(query("SELECT count(*) FROM p;"), (std::vector<std::string>{"(3)"}));
    }

    TEST_F(DialectTest, PredicatesInABranch) {
        createKeys();
        exec("CREATE BRANCH b FROM master;");
        EXPECT_EQ(exec("UPDATE p SET v = 'y' WHERE c BETWEEN 2 AND 5;", "b").rows, 2u);
        EXPECT_EQ(query("SELECT v FROM p WHERE v = 'y';", "b"), (std::vector<std::string>{"(y)", "(y)"}));
        EXPECT_TRUE(query("SELECT v FROM p WHERE v = 'y';").empty());
        EXPECT_THROW(exec("SELECT branch, count(*) FROM p VERSION (master, b) WHERE c > 1 GROUP BY branch;"), std::exception);
    }

    TEST_F(DialectTest, InsertOfSeveralRows) {
        exec("CREATE TABLE r ( a integer not null, b varchar(10) not null );");
        auto result = exec("INSERT INTO r (a, b) VALUES (1, 'one'), (2, 'two'), (3, 'three');");
        EXPECT_EQ(result.tag, "INSERT");
        EXPECT_EQ(result.rows, 3u);
        EXPECT_EQ(query("SELECT a, b FROM r;"), (std::vector<std::string>{"(1, one)", "(2, two)", "(3, three)"}));
        EXPECT_THROW(exec("INSERT INTO r (a, b) VALUES (4, 'four'), (5);"), std::exception);
        // a thousand rows compile into a loop, not into a call per row
        std::string many = "INSERT INTO r VALUES (0, 'x')";
        for (int i = 1; i < 1000; ++i) many += ", (" + std::to_string(i) + ", 'x')";
        EXPECT_EQ(exec(many + ";").rows, 1000u);
        EXPECT_EQ(query("SELECT count(*) FROM r;"), (std::vector<std::string>{"(1003)"}));
    }

    TEST_F(DialectTest, PrimaryKeyIsRecorded) {
        exec("CREATE TABLE k ( a integer not null, b integer not null, c integer not null, PRIMARY KEY (b, a) );");
        EXPECT_EQ(db->getTable("k")->getPrimaryKey(), (std::vector<std::string>{"b", "a"}));
        exec("CREATE TABLE l ( id integer PRIMARY KEY, v integer not null );");
        EXPECT_EQ(db->getTable("l")->getPrimaryKey(), (std::vector<std::string>{"id"}));
        EXPECT_THROW(exec("CREATE TABLE m ( a integer not null, PRIMARY KEY (z) );"), std::exception);
        EXPECT_THROW(exec("CREATE TABLE n ( a integer PRIMARY KEY, b integer, PRIMARY KEY (b) );"), std::exception);
    }

    TEST_F(DialectTest, TypesOfPostgreSQL) {
        exec("CREATE TABLE s ( a smallint, b int4, c bigint, d decimal(10,2), e float, f boolean, g varchar(300), "
             "h char(1), i char(3), j timestamp, k date );");
        Table & s = *db->getTable("s");
        using T = Sql::SqlType::TypeID;
        auto typeOf = [&](const std::string & c) { return s.getCI(c)->type; };
        EXPECT_EQ(typeOf("a").typeID, T::IntegerID);
        EXPECT_EQ(typeOf("b").typeID, T::IntegerID);
        EXPECT_EQ(typeOf("c").typeID, T::LongIntegerID);
        EXPECT_EQ(typeOf("d").typeID, T::NumericID);
        EXPECT_EQ(typeOf("e").typeID, T::NumericID); // float has no counterpart: numeric(18, 6)
        EXPECT_EQ(typeOf("e").numericLayout.precision, 6u);
        EXPECT_EQ(typeOf("f").typeID, T::BoolID);
        // declared nullable, stored not null: versions hold no null indicators
        EXPECT_FALSE(typeOf("a").nullable);
        exec("INSERT INTO s VALUES (1, 2, 9000000000, -3.25, 1000.5, true, 'a long text', 'q', 'abc', '2025-01-02T03:04:05.5', '2025-01-02');");
        EXPECT_EQ(query("SELECT a, c, d, e, g, h, i, j, k FROM s;"),
                  (std::vector<std::string>{"(1, 9000000000, -3.25, 1000.500000, a long text, q, abc, 2025-01-02 03:04:05.500, 2025-01-02)"}));
        EXPECT_EQ(query("SELECT a FROM s WHERE h = 'q' AND c > 8000000000;"), (std::vector<std::string>{"(1)"}));
        EXPECT_THROW(exec("INSERT INTO s VALUES (1, 2, 3, 1, 1, true, 'x', 'toolong', 'abc', '2025-01-01', '2025-01-01');"), std::exception);
        EXPECT_THROW(exec("INSERT INTO s VALUES (NULL, 2, 3, 1, 1, true, 'x', 'q', 'abc', '2025-01-01', '2025-01-01');"), std::exception);
    }

    TEST_F(DialectTest, LiteralsOfClients) {
        exec("CREATE TABLE q ( a integer not null, b varchar(40) not null, c timestamp not null );");
        // spaces, quotes, semicolons and parentheses in a literal; casts of psycopg2
        exec("INSERT INTO q VALUES (1, 'it''s a (test); really', '2025-01-01T12:00:00'::timestamp);");
        exec("INSERT INTO q VALUES (2::integer, 'b', '2025-01-02 00:00:00');");
        EXPECT_EQ(query("SELECT a, b, c FROM q;"),
                  (std::vector<std::string>{"(1, it's a (test); really, 2025-01-01 12:00:00)", "(2, b, 2025-01-02 00:00:00)"}));
        EXPECT_EQ(query("SELECT a FROM q WHERE b = 'it''s a (test); really';"), (std::vector<std::string>{"(1)"}));
    }

    TEST_F(DialectTest, ResultsDescribeThemselves) {
        exec("CREATE TABLE d ( a integer not null, b varchar(5) not null );");
        auto created = exec("CREATE TABLE e ( a integer not null );");
        EXPECT_EQ(created.tag, "CREATE TABLE");
        EXPECT_FALSE(created.returnsRows);
        exec("INSERT INTO d VALUES (1, 'x'), (2, 'y');");
        auto selected = exec("SELECT b, a FROM d WHERE a >= 1;");
        EXPECT_TRUE(selected.returnsRows);
        EXPECT_EQ(selected.rows, 2u);
        EXPECT_EQ(selected.columnNames, (std::vector<std::string>{"b", "a"}));
        EXPECT_EQ(selected.columnTypes[0].typeID, Sql::SqlType::TypeID::VarcharID);
        auto aggregated = exec("SELECT count(*), sum(a) FROM d;");
        EXPECT_EQ(aggregated.columnNames, (std::vector<std::string>{"count", "sum"}));
        EXPECT_EQ(exec("UPDATE d SET b = 'z' WHERE a = 2;").rows, 1u);
        EXPECT_EQ(exec("DELETE FROM d WHERE a = 7;").rows, 0u);
        // the session's branch is the default of relations without VERSION
        exec("CREATE BRANCH dev FROM master;");
        exec("DELETE FROM d WHERE a = 1;", "dev");
        EXPECT_EQ(query("SELECT count(*) FROM d;", "dev"), (std::vector<std::string>{"(1)"}));
        EXPECT_EQ(query("SELECT count(*) FROM d;", "main"), (std::vector<std::string>{"(2)"}));
    }

}
