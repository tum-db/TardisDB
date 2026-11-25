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
        // queries over several branches take any condition
        EXPECT_EQ(query("SELECT branch, count(*) FROM p VERSION (master, b) WHERE c > 1 GROUP BY branch;"),
                  (std::vector<std::string>{"(b, 2)", "(master, 2)"}));
        EXPECT_EQ(query("SELECT branch, count(*) FROM p VERSION (master, b) WHERE v = 'y' OR w = 1 GROUP BY branch;"),
                  (std::vector<std::string>{"(b, 4)", "(master, 3)"}));
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
        EXPECT_EQ(typeOf("e").typeID, T::DoubleID); // float is double precision
        EXPECT_EQ(typeOf("f").typeID, T::BoolID);
        // declared without NOT NULL: nullable
        EXPECT_TRUE(typeOf("a").nullable);
        exec("INSERT INTO s VALUES (1, 2, 9000000000, -3.25, 1000.5, true, 'a long text', 'q', 'abc', '2025-01-02T03:04:05.5', '2025-01-02');");
        EXPECT_EQ(query("SELECT a, c, d, e, g, h, i, j, k FROM s;"),
                  (std::vector<std::string>{"(1, 9000000000, -3.25, 1000.5, a long text, q, abc, 2025-01-02 03:04:05.500, 2025-01-02)"}));
        EXPECT_EQ(query("SELECT a FROM s WHERE h = 'q' AND c > 8000000000;"), (std::vector<std::string>{"(1)"}));
        EXPECT_THROW(exec("INSERT INTO s VALUES (1, 2, 3, 1, 1, true, 'x', 'toolong', 'abc', '2025-01-01', '2025-01-01');"), std::exception);
        exec("INSERT INTO s VALUES (NULL, 2, 3, 1, 1, true, 'x', 'q', 'abc', '2025-01-01', NULL);");
        EXPECT_EQ(query("SELECT a, c, k FROM s WHERE a IS NULL;"), (std::vector<std::string>{"(NULL, 3, NULL)"}));
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

    // text: = <> < <= > >= by the bytes, inline (up to 15 bytes) and in the string pool, nullable, and as keys of joins
    TEST_F(DialectTest, TextComparisons) {
        using rows_t = std::vector<std::string>;
        exec("CREATE TABLE tn ( t text not null, k integer not null );");
        exec("CREATE TABLE tu ( t text, k integer not null );");
        exec("INSERT INTO tn VALUES ('b', 1), ('a long text of the key', 2), ('c', 3), ('', 4);");
        exec("INSERT INTO tu VALUES ('b', 1), (NULL, 2), ('a long text of the key', 3);");
        EXPECT_EQ(query("SELECT k FROM tn WHERE t = 'b';"), (rows_t{"(1)"}));
        EXPECT_EQ(query("SELECT k FROM tn WHERE t = 'a long text of the key';"), (rows_t{"(2)"}));
        EXPECT_EQ(query("SELECT k FROM tn WHERE t = '';"), (rows_t{"(4)"}));
        EXPECT_EQ(query("SELECT k FROM tn WHERE t < 'b';"), (rows_t{"(2)", "(4)"}));
        EXPECT_EQ(query("SELECT k FROM tn WHERE t >= 'b';"), (rows_t{"(1)", "(3)"}));
        EXPECT_EQ(query("SELECT k FROM tn WHERE t > 'a long text';"), (rows_t{"(1)", "(2)", "(3)"}));
        EXPECT_EQ(query("SELECT k FROM tu WHERE t = 'b';"), (rows_t{"(1)"}));
        EXPECT_EQ(query("SELECT k FROM tu WHERE t <> 'b';"), (rows_t{"(3)"}));
        EXPECT_EQ(query("SELECT k FROM tu WHERE t > 'a';"), (rows_t{"(1)", "(3)"}));
        EXPECT_EQ(query("SELECT k FROM tu WHERE 'c' > t;"), (rows_t{"(1)", "(3)"}));
        EXPECT_EQ(query("SELECT tn.k, tu.k FROM tn, tu WHERE tn.t = tu.t;"), (rows_t{"(1, 1)", "(2, 3)"}));
    }

    // join conditions in either direction, in a cycle, and over expressions of one table on each side
    TEST_F(DialectTest, JoinConditions) {
        using rows_t = std::vector<std::string>;
        exec("CREATE TABLE ja ( id integer not null, k integer not null, name varchar(10) not null );");
        exec("CREATE TABLE jb ( id integer not null, k integer not null, y integer not null );");
        exec("CREATE TABLE jc ( id integer not null, k integer not null, z integer not null );");
        exec("CREATE TABLE jn ( id integer, v integer );");
        exec("CREATE TABLE jm ( m numeric(6, 2) not null );");
        exec("INSERT INTO ja VALUES (1, 1, 'one'), (2, 1, 'two'), (2, 2, 'twotwo'), (3, 1, 'three');");
        exec("INSERT INTO jb VALUES (1, 1, 100), (2, 1, 200), (2, 2, 220), (2, 2, 221), (4, 1, 400);");
        exec("INSERT INTO jc VALUES (1, 1, 1000), (2, 9, 2000), (2, 2, 2200);");
        exec("INSERT INTO jn VALUES (1, 5), (NULL, 6), (2, NULL);");
        exec("INSERT INTO jm VALUES (2.00), (4.50);");
        // the second condition names the tables the other way round
        EXPECT_EQ(query("SELECT ja.name, jb.y FROM ja, jb WHERE ja.id = jb.id AND jb.k = ja.k;"),
                  (rows_t{"(one, 100)", "(two, 200)", "(twotwo, 220)", "(twotwo, 221)"}));
        // a cycle: the last condition is over two tables that are joined already
        EXPECT_EQ(query("SELECT ja.name, jb.y, jc.z FROM ja, jb, jc WHERE ja.id = jb.id AND jb.id = jc.id AND ja.k = jc.k;"),
                  (rows_t{"(one, 100, 1000)", "(twotwo, 200, 2200)", "(twotwo, 220, 2200)", "(twotwo, 221, 2200)"}));
        // expressions, in both directions and with a condition on columns
        const rows_t plusTwo{"(two, 400)", "(twotwo, 400)"};
        EXPECT_EQ(query("SELECT ja.name, jb.y FROM ja, jb WHERE ja.id + 2 = jb.id;"), plusTwo);
        EXPECT_EQ(query("SELECT ja.name, jb.y FROM ja, jb WHERE jb.id = ja.id + 2;"), plusTwo);
        EXPECT_EQ(query("SELECT ja.name, jb.y FROM ja, jb WHERE jb.id - 1 = ja.id AND ja.k = jb.k;"), (rows_t{"(one, 200)", "(three, 400)"}));
        EXPECT_EQ(query("SELECT ja.name, jn.v FROM ja, jn WHERE ja.id = jn.id + 0;"), (rows_t{"(one, 5)", "(two, NULL)", "(twotwo, NULL)"}));
        // sides of different types: keys in their common type (numeric)
        EXPECT_EQ(query("SELECT ja.name FROM ja, jm WHERE ja.id + 0 = jm.m + 0;"), (rows_t{"(two)", "(twotwo)"}));
        // the hash join applies the condition over expressions: no filter above it
        std::string plan;
        QueryCompiler::execute("EXPLAIN SELECT ja.name, jb.y FROM ja, jb WHERE ja.id + 2 = jb.id;", *db, "master",
                               [&](Native::Sql::SqlTuple * tuple) { plan = Native::Sql::toString(*tuple); });
        EXPECT_NE(plan.find("Hash Join"), std::string::npos) << plan;
        EXPECT_EQ(plan.find("Filter"), std::string::npos) << plan;
    }

    TEST_F(DialectTest, JoinsOfTables) {
        exec("CREATE TABLE ja ( id integer not null, k integer not null, name varchar(10) not null );");
        exec("CREATE TABLE jb ( id integer not null, k integer not null, y integer not null );");
        exec("CREATE TABLE jc ( name varchar(10) not null, z integer not null );");
        exec("CREATE TABLE jn ( id integer, v integer );");
        exec("INSERT INTO ja VALUES (1, 1, 'one'), (2, 1, 'two'), (2, 2, 'twotwo'), (3, 1, 'three');");
        exec("INSERT INTO jb VALUES (1, 1, 100), (2, 1, 200), (2, 2, 220), (2, 2, 221), (4, 1, 400);");
        exec("INSERT INTO jc VALUES ('one', 1000), ('twotwo', 2200);");
        exec("INSERT INTO jn VALUES (1, 5), (NULL, 6), (2, NULL);");
        // several matches per key, keys of two columns, three tables, text keys
        EXPECT_EQ(query("SELECT ja.name, jb.y FROM ja, jb WHERE ja.id = jb.id AND ja.k = jb.k;"),
                  (std::vector<std::string>{"(one, 100)", "(two, 200)", "(twotwo, 220)", "(twotwo, 221)"}));
        EXPECT_EQ(query("SELECT ja.name, jb.y, jc.z FROM ja, jb, jc WHERE ja.id = jb.id AND ja.k = jb.k AND ja.name = jc.name;"),
                  (std::vector<std::string>{"(one, 100, 1000)", "(twotwo, 220, 2200)", "(twotwo, 221, 2200)"}));
        EXPECT_EQ(query("SELECT ja.name, jb.y FROM ja, jb WHERE ja.id = jb.id AND jb.y > 210 AND ja.k = 1;"),
                  (std::vector<std::string>{"(two, 220)", "(two, 221)"}));
        // tables are bound by their names or aliases; unqualified columns must belong to one table
        EXPECT_EQ(query("SELECT y, z FROM ja a, jb, jc WHERE a.id = jb.id AND a.k = jb.k AND a.name = jc.name AND y > 220;"),
                  (std::vector<std::string>{"(221, 2200)"}));
        EXPECT_THROW(exec("SELECT name FROM ja, jc WHERE ja.name = jc.name;"), std::exception);
        EXPECT_THROW(exec("SELECT ja.y FROM ja, jb WHERE ja.id = jb.id;"), std::exception);
        // tables that no condition joins: cross products
        EXPECT_EQ(query("SELECT ja.name, jc.z FROM ja, jc WHERE ja.id = 1;"), (std::vector<std::string>{"(one, 1000)", "(one, 2200)"}));
        EXPECT_EQ(query("SELECT ja.id, jb.y, jc.z FROM ja, jb, jc WHERE ja.id = jb.id AND ja.k = 2 AND jb.y > 220;"),
                  (std::vector<std::string>{"(2, 221, 1000)", "(2, 221, 2200)"}));
        EXPECT_EQ(query("SELECT ja.name, jb.y FROM ja, jb WHERE ja.id < jb.id AND jb.y = 400;"),
                  (std::vector<std::string>{"(one, 400)", "(three, 400)", "(two, 400)", "(twotwo, 400)"}));
        // an empty side, which the selections leave
        EXPECT_TRUE(query("SELECT ja.name, jb.y FROM ja, jb WHERE ja.id = jb.id AND ja.k = 9;").empty());
        EXPECT_TRUE(query("SELECT ja.name, jb.y FROM ja, jb WHERE ja.id = jb.id AND jb.k = 9;").empty());
        // NULL matches no key, not even NULL
        EXPECT_EQ(query("SELECT ja.name, jn.v FROM ja, jn WHERE ja.id = jn.id;"),
                  (std::vector<std::string>{"(one, 5)", "(two, NULL)", "(twotwo, NULL)"}));
        EXPECT_EQ(query("SELECT x.v, y.v FROM jn x, jn y WHERE x.id = y.id;"),
                  (std::vector<std::string>{"(5, 5)", "(NULL, NULL)"}));
        // the tables of a join in a branch, and two versions of one table
        exec("CREATE BRANCH jdev FROM master;");
        exec("UPDATE ja VERSION jdev SET name = 'uno' WHERE id = 1;");
        exec("INSERT INTO jc VERSION jdev VALUES ('uno', 1001);");
        EXPECT_EQ(query("SELECT ja.name, jc.z FROM ja VERSION jdev ja, jc VERSION jdev jc WHERE ja.name = jc.name;"),
                  (std::vector<std::string>{"(twotwo, 2200)", "(uno, 1001)"}));
        EXPECT_EQ(query("SELECT o.name, n.name FROM ja VERSION master o, ja VERSION jdev n WHERE o.id = n.id AND o.k = n.k AND o.id = 1;"),
                  (std::vector<std::string>{"(one, uno)"}));
    }

}
