// The index join on a BranchIndex (an adaptive radix tree): the rows of the hash join, the plan, keys of
// several types, NULL keys, several matches, an index of either side, more pairs and selections, a branch
// whose index holds stale entries after updates and deletes, inserts after CREATE INDEX, three tables, the
// size rule and the setting; and the keys of the index

#include "codegen/LLVMCompat.hpp"
#include <algorithm>
#include <mutex>
#include "foundations/Database.hpp"
#include "native/sql/SqlValues.hpp"
#include "native/sql/SqlTuple.hpp"
#include "queryCompiler/queryCompiler.hpp"
#include <gtest/gtest.h>

namespace {

    using rows_t = std::vector<std::string>;

    class IndexJoinTest : public ::testing::Test {
    protected:
        void SetUp() override {
            db = std::make_unique<Database>();
            exec("CREATE TABLE ic ( id integer not null, name varchar(20) not null, t text, d double precision );");
            exec("CREATE TABLE io ( id integer not null, c integer, note text, d double precision );");
            exec("CREATE TABLE ie ( id integer not null );");
            exec("INSERT INTO ic VALUES (1, 'alice', 'a long text key of alice', 1.5), (2, 'bob', 'b', -0.0), "
                 "(3, 'carol', NULL, NULL), (2, 'bob2', 'b', 2.5);");
            exec("INSERT INTO io VALUES (10, 1, 'a long text key of alice', 1.5), (11, 2, 'b', 0.0), (12, 2, 'x', NULL), "
                 "(13, NULL, NULL, 2.5);");
            // tuples that join nothing: the indexed table has more than 32 times the tuples of the probe side
            std::string filler = "INSERT INTO ic VALUES (1000, 'f', NULL, NULL)";
            for (int i = 1001; i < 1200; ++i) filler += ", (" + std::to_string(i) + ", 'f', NULL, NULL)";
            exec(filler + ";");
            exec("CREATE INDEX ON ic (id);");
            exec("CREATE INDEX ON ic (t);");
            exec("CREATE INDEX ON ic (d);");
            exec("CREATE INDEX ON ie (id);");
        }

        void TearDown() override { QueryCompiler::setSetting("enable_indexjoin", "on"); }

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

        std::string plan(const std::string & sql) {
            std::string text;
            QueryCompiler::execute("EXPLAIN " + sql, *db, "master", [&](Native::Sql::SqlTuple * tuple) { text = Native::Sql::toString(*tuple); });
            return text;
        }

        /// \returns The rows of the join by the index, sorted, which the hash join gives as well
        rows_t join(const std::string & sql) {
            EXPECT_NE(plan(sql).find("Index Join"), std::string::npos) << plan(sql);
            const rows_t indexed = run(sql);
            QueryCompiler::setSetting("enable_indexjoin", "off");
            EXPECT_EQ(plan(sql).find("Index Join"), std::string::npos) << sql;
            EXPECT_EQ(indexed, run(sql)) << sql;
            QueryCompiler::setSetting("enable_indexjoin", "on");
            return indexed;
        }

        std::unique_ptr<Database> db;
    };

    TEST_F(IndexJoinTest, Keys) {
        // several matches, NULL keys, an unmatched key
        EXPECT_EQ(join("SELECT io.id, ic.name FROM io, ic WHERE io.c = ic.id;"),
                  (rows_t{"(10, alice)", "(11, bob)", "(11, bob2)", "(12, bob)", "(12, bob2)"}));
        // text, inline and in the string pool; double, -0 = 0
        EXPECT_EQ(join("SELECT io.id, ic.name FROM io, ic WHERE io.note = ic.t;"), (rows_t{"(10, alice)", "(11, bob)", "(11, bob2)"}));
        EXPECT_EQ(join("SELECT io.id, ic.name FROM io, ic WHERE io.d = ic.d;"), (rows_t{"(10, alice)", "(11, bob)", "(13, bob2)"}));
        // the plan names the index
        EXPECT_NE(plan("SELECT io.id FROM io, ic WHERE io.c = ic.id;").find("ic_id_master"), std::string::npos);
    }

    TEST_F(IndexJoinTest, Shapes) {
        // the indexed table on the left, more pairs, selections on both sides, an expression of the probe side
        EXPECT_EQ(join("SELECT io.id, ic.name FROM ic, io WHERE ic.id = io.c;"),
                  (rows_t{"(10, alice)", "(11, bob)", "(11, bob2)", "(12, bob)", "(12, bob2)"}));
        EXPECT_EQ(join("SELECT io.id, ic.name FROM io, ic WHERE io.c = ic.id AND io.note = ic.t;"), (rows_t{"(10, alice)", "(11, bob)", "(11, bob2)"}));
        EXPECT_EQ(join("SELECT io.id, ic.name FROM io, ic WHERE io.c = ic.id AND ic.name <> 'bob2' AND io.id > 10;"), (rows_t{"(11, bob)", "(12, bob)"}));
        EXPECT_EQ(join("SELECT io.id, ic.name FROM io, ic WHERE io.c + 1 = ic.id;"), (rows_t{"(10, bob)", "(10, bob2)", "(11, carol)", "(12, carol)"}));
        // three tables: the index join probes with the rows of a join
        EXPECT_EQ(join("SELECT a.id, b.id, ic.name FROM io a, io b, ic WHERE a.c = b.c AND b.c = ic.id AND a.id = 10;"), (rows_t{"(10, 10, alice)"}));
        // an empty probe side
        EXPECT_EQ(join("SELECT ie.id FROM ie, ic WHERE ie.id = ic.id;"), rows_t{});
    }

    TEST_F(IndexJoinTest, Writes) {
        // inserts after CREATE INDEX are in the index
        exec("INSERT INTO ic VALUES (12, 'dave', 'x', NULL);");
        EXPECT_EQ(join("SELECT io.id, ic.name FROM io, ic WHERE io.id = ic.id;"), (rows_t{"(12, dave)"}));
        // a branch: its own index, stale entries after an update and a delete
        exec("CREATE BRANCH dev FROM master;");
        exec("UPDATE ic VERSION dev SET id = 7 WHERE name = 'bob2';");
        exec("CREATE INDEX ON ic (id) VERSION dev;");
        exec("UPDATE ic VERSION dev SET id = 1 WHERE name = 'carol';");
        exec("DELETE FROM ic VERSION dev WHERE name = 'alice';");
        const std::string query = "SELECT io.id, ic.name FROM io, ic VERSION dev WHERE io.c = ic.id;";
        EXPECT_NE(plan(query).find("ic_id_dev"), std::string::npos) << plan(query);
        EXPECT_EQ(join(query), (rows_t{"(10, carol)", "(11, bob)", "(12, bob)"}));
        // master is unchanged
        EXPECT_EQ(join("SELECT io.id, ic.name FROM io, ic WHERE io.c = ic.id;"),
                  (rows_t{"(10, alice)", "(11, bob)", "(11, bob2)", "(12, bob)", "(12, bob2)"}));
    }

    TEST_F(IndexJoinTest, Choice) {
        // a probe side with more than 1/32 of the tuples of the indexed table: the hash join
        exec("CREATE TABLE ism ( id integer not null );");
        exec("INSERT INTO ism VALUES (1);");
        exec("CREATE INDEX ON ism (id);");
        EXPECT_EQ(plan("SELECT io.id FROM io, ism WHERE io.c = ism.id;").find("Index Join"), std::string::npos);
        EXPECT_EQ(run("SELECT io.id FROM io, ism WHERE io.c = ism.id;"), (rows_t{"(10)"}));
        // an expression of the indexed column, which the hash join takes
        EXPECT_EQ(plan("SELECT io.id FROM io, ic WHERE io.c = ic.id + 0;").find("Index Join"), std::string::npos);
        EXPECT_EQ(QueryCompiler::getSetting("enable_indexjoin"), "on");
        QueryCompiler::setSetting("enable_indexjoin", "off");
        EXPECT_EQ(QueryCompiler::getSetting("enable_indexjoin"), "off");
    }

    TEST_F(IndexJoinTest, IndexKeys) {
        auto key = [](const std::string & text, Sql::SqlType type) {
            return BranchIndex::key(*Native::Sql::Value::castString(text, type));
        };
        // numbers in their order, -0 = 0, NULL has no key
        EXPECT_LT(key("-5", Sql::getIntegerTy()), key("3", Sql::getIntegerTy()));
        EXPECT_LT(key("-3000000000", Sql::getLongIntegerTy()), key("-1", Sql::getLongIntegerTy()));
        EXPECT_LT(key("-1.5", Sql::getDoubleTy()), key("0.25", Sql::getDoubleTy()));
        EXPECT_EQ(key("-0.0", Sql::getDoubleTy()), key("0", Sql::getDoubleTy()));
        EXPECT_TRUE(BranchIndex::key(*Native::Sql::castColumnValue(Native::Sql::nullLiteral, Sql::getIntegerTy(true))).empty());
        // strings: no key is a prefix of another
        const std::string ab = key("ab", Sql::getTextTy()), abc = key("abc", Sql::getTextTy());
        EXPECT_NE(abc.compare(0, ab.size(), ab), 0);
        EXPECT_LT(ab, abc);
    }

} // namespace
