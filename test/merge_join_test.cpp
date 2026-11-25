// The sort-merge join (SET enable_hashjoin = off): the same rows as the hash join for keys of each type,
// several matches on both sides, keys of several columns and of expressions, NULL keys, empty sides,
// three tables, and the plan

#include "codegen/LLVMCompat.hpp"
#include <algorithm>
#include <mutex>
#include "algebra/translation.hpp"
#include "foundations/Parallel.hpp"
#include "native/sql/SqlTuple.hpp"
#include "queryCompiler/queryCompiler.hpp"
#include <gtest/gtest.h>

namespace {

    using rows_t = std::vector<std::string>;

    class MergeJoinTest : public ::testing::Test {
    protected:
        void SetUp() override {
            db = std::make_unique<Database>();
            exec("CREATE TABLE ja ( id integer not null, k integer not null, name varchar(10) not null, t text, d double precision, n numeric(6, 2) );");
            exec("CREATE TABLE jb ( id integer not null, k integer not null, y integer not null, t text, d double precision, n numeric(6, 2) );");
            exec("CREATE TABLE jc ( name varchar(10) not null, z integer not null );");
            exec("CREATE TABLE jn ( id integer, v integer );");
            exec("INSERT INTO ja VALUES (1, 1, 'one', 'a long text of the key', 1.5, 1.10), (2, 1, 'two', 'b', 2.5, 2.20), "
                 "(2, 2, 'twotwo', 'b', NULL, 2.20), (3, 1, 'three', NULL, -0.0, NULL), (2, 2, 'again', 'c', 2.5, 3.30);");
            exec("INSERT INTO jb VALUES (1, 1, 100, 'a long text of the key', 1.5, 1.10), (2, 1, 200, 'b', 2.5, 2.20), "
                 "(2, 2, 220, 'b', 0.0, 2.20), (2, 2, 221, NULL, 2.5, NULL), (4, 1, 400, 'c', NULL, 3.30);");
            exec("INSERT INTO jc VALUES ('one', 1000), ('twotwo', 2200), ('again', 3000);");
            exec("INSERT INTO jn VALUES (1, 5), (NULL, 6), (2, NULL), (NULL, 7);");
        }

        void TearDown() override { QueryCompiler::setSetting("enable_hashjoin", "on"); }

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
            std::sort(rows.begin(), rows.end());
            return rows;
        }

        /// \returns The rows of the query with the merge join, sorted, which the hash join gives as well
        rows_t query(const std::string & sql) {
            QueryCompiler::setSetting("enable_hashjoin", "off");
            const rows_t merged = run(sql);
            QueryCompiler::setSetting("enable_hashjoin", "on");
            EXPECT_EQ(merged, run(sql)) << sql;
            return merged;
        }

        std::string plan(const std::string & sql) {
            QueryCompiler::setSetting("enable_hashjoin", "off");
            std::string text;
            QueryCompiler::execute("EXPLAIN " + sql, *db, "master", [&](Native::Sql::SqlTuple * tuple) { text = Native::Sql::toString(*tuple); });
            QueryCompiler::setSetting("enable_hashjoin", "on");
            return text;
        }

        std::unique_ptr<Database> db;
    };

    TEST_F(MergeJoinTest, Keys) {
        // several matches on both sides, two columns
        EXPECT_EQ(query("SELECT ja.name, jb.y FROM ja, jb WHERE ja.id = jb.id;"),
                  (rows_t{"(again, 200)", "(again, 220)", "(again, 221)", "(one, 100)", "(two, 200)", "(two, 220)", "(two, 221)",
                          "(twotwo, 200)", "(twotwo, 220)", "(twotwo, 221)"}));
        EXPECT_EQ(query("SELECT ja.name, jb.y FROM ja, jb WHERE ja.id = jb.id AND jb.k = ja.k;"),
                  (rows_t{"(again, 220)", "(again, 221)", "(one, 100)", "(two, 200)", "(twotwo, 220)", "(twotwo, 221)"}));
        // the types of keys: varchar, text (also longer than 15 bytes), double (-0 = 0), numeric
        EXPECT_EQ(query("SELECT ja.name, jc.z FROM ja, jc WHERE ja.name = jc.name;"), (rows_t{"(again, 3000)", "(one, 1000)", "(twotwo, 2200)"}));
        EXPECT_EQ(query("SELECT ja.id, jb.y FROM ja, jb WHERE ja.t = jb.t;"),
                  (rows_t{"(1, 100)", "(2, 200)", "(2, 200)", "(2, 220)", "(2, 220)", "(2, 400)"}));
        EXPECT_EQ(query("SELECT ja.id, jb.y FROM ja, jb WHERE ja.d = jb.d;"), (rows_t{"(1, 100)", "(2, 200)", "(2, 200)", "(2, 221)", "(2, 221)", "(3, 220)"}));
        EXPECT_EQ(query("SELECT ja.id, jb.y FROM ja, jb WHERE ja.n = jb.n;"),
                  (rows_t{"(1, 100)", "(2, 200)", "(2, 200)", "(2, 220)", "(2, 220)", "(2, 400)"}));
        // expressions
        EXPECT_EQ(query("SELECT ja.name, jb.y FROM ja, jb WHERE ja.id + 2 = jb.id;"), (rows_t{"(again, 400)", "(two, 400)", "(twotwo, 400)"}));
        EXPECT_EQ(query("SELECT ja.name, jb.y FROM ja, jb WHERE jb.id - 1 = ja.id AND ja.k = jb.k;"), (rows_t{"(one, 200)", "(three, 400)"}));
    }

    TEST_F(MergeJoinTest, NullsEmptySidesAndThreeTables) {
        // NULL matches no key, not even NULL
        EXPECT_EQ(query("SELECT ja.name, jn.v FROM ja, jn WHERE ja.id = jn.id;"),
                  (rows_t{"(again, NULL)", "(one, 5)", "(two, NULL)", "(twotwo, NULL)"}));
        EXPECT_EQ(query("SELECT x.v, y.v FROM jn x, jn y WHERE x.id = y.id;"), (rows_t{"(5, 5)", "(NULL, NULL)"}));
        // an empty side
        EXPECT_TRUE(query("SELECT ja.name, jb.y FROM ja, jb WHERE ja.id = jb.id AND ja.k = 9;").empty());
        EXPECT_TRUE(query("SELECT ja.name, jb.y FROM ja, jb WHERE ja.id = jb.id AND jb.k = 9;").empty());
        exec("CREATE TABLE je ( id integer not null );");
        EXPECT_TRUE(query("SELECT ja.name FROM ja, je WHERE ja.id = je.id;").empty());
        // three tables, a cycle of conditions, and a join without a condition next to a merge join
        EXPECT_EQ(query("SELECT ja.name, jb.y, jc.z FROM ja, jb, jc WHERE ja.id = jb.id AND ja.k = jb.k AND ja.name = jc.name;"),
                  (rows_t{"(again, 220, 3000)", "(again, 221, 3000)", "(one, 100, 1000)", "(twotwo, 220, 2200)", "(twotwo, 221, 2200)"}));
        EXPECT_EQ(query("SELECT ja.name, jb.y FROM ja, jb, jc WHERE ja.id = jb.id AND jb.k = ja.k AND ja.name = jc.name AND jc.z = jb.y * 10;"),
                  (rows_t{"(one, 100)", "(twotwo, 220)"}));
        EXPECT_EQ(query("SELECT ja.name, jc.z FROM ja, jb, jc WHERE ja.id = jb.id AND jb.y = 100;"),
                  (rows_t{"(one, 1000)", "(one, 2200)", "(one, 3000)"}));
        // with ORDER BY and aggregates above it
        EXPECT_EQ(query("SELECT ja.k, count(*) FROM ja, jb WHERE ja.id = jb.id GROUP BY ja.k;"), (rows_t{"(1, 4)", "(2, 6)"}));
    }

    TEST_F(MergeJoinTest, Plan) {
        const std::string p = plan("SELECT ja.name, jb.y FROM ja, jb WHERE ja.id + 2 = jb.id;");
        EXPECT_NE(p.find("\"Node Type\": \"Merge Join\""), std::string::npos) << p;
        EXPECT_NE(p.find("\"Merge Cond\""), std::string::npos) << p;
        EXPECT_EQ(p.find("Hash Join"), std::string::npos) << p;
        // the hash join by default, and for a join without keys
        EXPECT_NE(plan("SELECT ja.name, jc.z FROM ja, jc WHERE ja.id = 1;").find("Hash Join"), std::string::npos);
        std::string text;
        QueryCompiler::execute("EXPLAIN SELECT ja.name FROM ja, jb WHERE ja.id = jb.id;", *db, "master",
                               [&](Native::Sql::SqlTuple * tuple) { text = Native::Sql::toString(*tuple); });
        EXPECT_NE(text.find("Hash Join"), std::string::npos) << text;
    }

    TEST_F(MergeJoinTest, Settings) {
        EXPECT_EQ(QueryCompiler::getSetting("enable_hashjoin"), "on");
        EXPECT_TRUE(QueryCompiler::setSetting("ENABLE_HASHJOIN", "off"));
        EXPECT_EQ(Algebra::joinMethod(), Algebra::JoinMethod::Merge);
        EXPECT_TRUE(QueryCompiler::setSetting("enable_mergejoin", "off")); // both off: the hash join, as the last resort
        EXPECT_EQ(Algebra::joinMethod(), Algebra::JoinMethod::Hash);
        EXPECT_TRUE(QueryCompiler::setSetting("enable_mergejoin", "on"));
        EXPECT_TRUE(QueryCompiler::setSetting("enable_hashjoin", "'on'"));
        EXPECT_EQ(Algebra::joinMethod(), Algebra::JoinMethod::Hash);
        EXPECT_FALSE(QueryCompiler::setSetting("work_mem", "64MB"));
        EXPECT_THROW(QueryCompiler::setSetting("enable_hashjoin", "maybe"), std::invalid_argument);
        // in the sql shell
        QueryCompiler::compileAndExecute("SET enable_hashjoin = off;", *db);
        EXPECT_EQ(QueryCompiler::getSetting("enable_hashjoin"), "off");
        QueryCompiler::compileAndExecute("set enable_hashjoin to on", *db);
        EXPECT_EQ(QueryCompiler::getSetting("enable_hashjoin"), "on");
    }
}
