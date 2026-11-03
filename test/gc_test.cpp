// Versions that no branch needs are collected: create-and-drop cycles keep the number of versions
// bounded, while the remaining branches still read their versions and merge against their bases

#include "codegen/LLVMCompat.hpp"

#include <algorithm>

#include "foundations/version_management.hpp"
#include "native/sql/SqlTuple.hpp"
#include "queryCompiler/queryCompiler.hpp"
#include <gtest/gtest.h>

namespace {

    std::vector<std::string> rows;

    void collect(Native::Sql::SqlTuple * tuple) {
        rows.push_back(Native::Sql::toString(*tuple));
    }

    std::vector<std::string> query(Database & db, const std::string & sql) {
        rows.clear();
        QueryCompiler::compileAndExecute(sql, db, (void *) &collect);
        std::sort(rows.begin(), rows.end());
        return rows;
    }

    TEST(GarbageCollectionTest, CreateAndDropCycles) {
        Database db;
        QueryCompiler::compileAndExecute("create table t ( k integer not null , x integer not null );", db);
        for (int k = 0; k < 20; ++k) {
            QueryCompiler::compileAndExecute("insert into t ( k , x ) values ( " + std::to_string(k) + " , 0 );", db);
        }
        // a long-lived branch keeps the versions it sees
        QueryCompiler::compileAndExecute("create branch keep from main;", db);
        const size_t before = live_versions();
        for (int cycle = 1; cycle <= 50; ++cycle) {
            QueryCompiler::compileAndExecute("create branch c from main;", db);
            QueryCompiler::compileAndExecute("update t version c set x = " + std::to_string(cycle) + " where k = 1;", db);
            QueryCompiler::compileAndExecute("update t version main set x = " + std::to_string(cycle) + " where k = 2;", db);
            QueryCompiler::compileAndExecute("drop branch c;", db);
        }
        // main's in-place writes keep one version of k = 2 for keep, nothing for the dropped branches
        EXPECT_LE(live_versions(), before + 1);
        EXPECT_EQ(query(db, "select x from t version keep where k = 2;"), std::vector<std::string>({"(0)"}));
        EXPECT_EQ(query(db, "select x from t version main where k = 2;"), std::vector<std::string>({"(50)"}));
        EXPECT_EQ(query(db, "select x from t version main where k = 1;"), std::vector<std::string>({"(0)"}));
    }

    TEST(GarbageCollectionTest, MergeBaseSurvives) {
        Database db;
        QueryCompiler::compileAndExecute("create table t ( k integer not null , x integer not null , y integer not null );", db);
        QueryCompiler::compileAndExecute("insert into t ( k , x , y ) values ( 1 , 0 , 0 );", db);
        QueryCompiler::compileAndExecute("create branch a from main;", db);
        QueryCompiler::compileAndExecute("create branch b from main;", db);
        // a and b overwrite the version they share; a third branch is dropped
        QueryCompiler::compileAndExecute("update t version a set x = 1 where k = 1;", db);
        QueryCompiler::compileAndExecute("update t version b set y = 2 where k = 1;", db);
        QueryCompiler::compileAndExecute("update t version main set x = 0 , y = 0 where k = 1;", db);
        QueryCompiler::compileAndExecute("create branch d from a;", db);
        QueryCompiler::compileAndExecute("update t version d set x = 5 where k = 1;", db);
        QueryCompiler::compileAndExecute("drop branch d;", db);
        // the shared version is the base of the merge: x from a and y from b do not conflict
        QueryCompiler::compileAndExecute("merge branch a into b;", db);
        EXPECT_EQ(query(db, "select x , y from t version b;"), std::vector<std::string>({"(1, 2)"}));
    }

}
