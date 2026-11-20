// NULL values: nullable columns hold a null indicator beside each value, in place and in every version.
// Inserts, updates, the three-valued conditions of WHERE, aggregates that skip NULLs, branches that
// write NULLs, MERGE and DIFF, columns added by ALTER TABLE, COPY, and the checks of NOT NULL

#include "codegen/LLVMCompat.hpp"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <functional>
#include <mutex>
#include "foundations/Parallel.hpp"
#include "native/sql/SqlTuple.hpp"
#include "queryCompiler/queryCompiler.hpp"
#include "semanticAnalyser/SemanticAnalyser.hpp"
#include <gtest/gtest.h>

namespace {

    class NullTest : public ::testing::Test {
    protected:
        void SetUp() override {
            db = std::make_unique<Database>();
            exec("create table t ( k integer not null , x integer , s varchar ( 8 ) , d numeric ( 8 , 2 ) );");
            exec("insert into t ( k , x , s , d ) values ( 1 , 10 , 'a' , 1.50 ) , ( 2 , null , 'b' , null ) , ( 3 , 30 , null , 3.25 );");
        }

        void exec(const std::string & sql, const std::string & branch = "main") {
            QueryCompiler::execute(sql, *db, branch, [](Native::Sql::SqlTuple *) { });
        }

        /// \returns The rows of the query, sorted
        std::vector<std::string> query(const std::string & sql, const std::string & branch = "main") {
            std::vector<std::string> rows;
            std::mutex mutex;
            QueryCompiler::execute(sql, *db, branch, [&](Native::Sql::SqlTuple * tuple) {
                std::lock_guard<std::mutex> lock(mutex);
                rows.push_back(Native::Sql::toString(*tuple));
            });
            std::sort(rows.begin(), rows.end());
            return rows;
        }

        using rows_t = std::vector<std::string>;
        std::unique_ptr<Database> db;
    };

    TEST_F(NullTest, InsertsAndReadsNulls) {
        exec("insert into t ( k ) values ( 4 );"); // the columns not given are NULL
        EXPECT_EQ(query("select k , x , s , d from t;"),
                  (rows_t{"(1, 10, a, 1.50)", "(2, NULL, b, NULL)", "(3, 30, NULL, 3.25)", "(4, NULL, NULL, NULL)"}));
        EXPECT_THROW(exec("insert into t ( k , x ) values ( null , 1 );"), semanticalAnalysis::not_null_violation);
        EXPECT_EQ(query("select k from t where k = 4;"), (rows_t{"(4)"}));
    }

    // a comparison with NULL is NULL, which WHERE treats as false; IS [NOT] NULL is never NULL
    TEST_F(NullTest, ThreeValuedConditions) {
        EXPECT_EQ(query("select k from t where x is null;"), (rows_t{"(2)"}));
        EXPECT_EQ(query("select k from t where x is not null;"), (rows_t{"(1)", "(3)"}));
        EXPECT_EQ(query("select k from t where x = 30;"), (rows_t{"(3)"}));
        EXPECT_EQ(query("select k from t where x <> 30;"), (rows_t{"(1)"}));
        EXPECT_EQ(query("select k from t where x < 100;"), (rows_t{"(1)", "(3)"}));
        EXPECT_EQ(query("select k from t where x between 0 and 20;"), (rows_t{"(1)"}));
        EXPECT_EQ(query("select k from t where x = null;"), (rows_t{}));
        EXPECT_EQ(query("select k from t where null = x;"), (rows_t{}));
        EXPECT_EQ(query("select k from t where x <> null;"), (rows_t{}));
        EXPECT_EQ(query("select k from t where x in ( 30 , null );"), (rows_t{"(3)"}));
        EXPECT_EQ(query("select k from t where s = 'b';"), (rows_t{"(2)"}));
        EXPECT_EQ(query("select k from t where s is null and x is not null;"), (rows_t{"(3)"}));
        // row values: ( 2 , NULL ) >= ( 2 , 0 ) is NULL, ( 3 , 30 ) is greater
        EXPECT_EQ(query("select k from t where ( k , x ) >= ( 2 , 0 );"), (rows_t{"(3)"}));
        EXPECT_EQ(query("select k from t where ( k , x ) < ( 2 , 0 );"), (rows_t{"(1)"}));
    }

    TEST_F(NullTest, UpdatesToAndFromNull) {
        exec("update t set x = x + 1;"); // NULL + 1 is NULL
        EXPECT_EQ(query("select k , x from t;"), (rows_t{"(1, 11)", "(2, NULL)", "(3, 31)"}));
        exec("update t set x = null , s = null where k = 1;");
        exec("update t set x = 5 where x is null;");
        EXPECT_EQ(query("select k , x , s from t;"), (rows_t{"(1, 5, NULL)", "(2, 5, b)", "(3, 31, NULL)"}));
        exec("update t set d = d * 2;");
        EXPECT_EQ(query("select k , d from t;"), (rows_t{"(1, 3.00)", "(2, NULL)", "(3, 6.50)"}));
        // a column that is NOT NULL takes neither NULL nor a value that may be NULL
        EXPECT_THROW(exec("update t set k = null;"), semanticalAnalysis::not_null_violation);
        EXPECT_THROW(exec("update t set k = x;"), semanticalAnalysis::not_null_violation);
        exec("update t set x = k * 10;"); // a value of a column without nulls for a nullable one
        EXPECT_EQ(query("select k , x from t;"), (rows_t{"(1, 10)", "(2, 20)", "(3, 30)"}));
    }

    // aggregates skip NULLs; count(x) counts the values; sum, min, max and avg of no values are NULL
    TEST_F(NullTest, AggregatesSkipNulls) {
        EXPECT_EQ(query("select count(*) , count(x) , sum(x) , min(x) , max(x) , avg(x) from t;"),
                  (rows_t{"(3, 2, 40, 10, 30, 20)"}));
        EXPECT_EQ(query("select sum(d) , avg(d) from t;"), (rows_t{"(4.75, 2.37)"}));
        exec("update t set x = null;");
        EXPECT_EQ(query("select count(*) , count(x) , sum(x) , min(x) , max(x) , avg(x) from t;"),
                  (rows_t{"(3, 0, NULL, NULL, NULL, NULL)"}));
        EXPECT_EQ(query("select count(x) from t where k = 1;"), (rows_t{"(0)"}));
    }

    // a branch writes NULLs in its versions, which neither main nor its siblings see; the readers of a
    // cross-branch aggregate skip the NULLs each of them sees, in the shared scan and per branch
    TEST_F(NullTest, BranchesWriteNulls) {
        exec("create branch b from main;");
        exec("create branch c from main;");
        exec("update t version b set x = null where k = 1;");
        exec("update t version b set x = 20 where k = 2;");
        exec("insert into t version b ( k ) values ( 4 );");
        exec("update t version c set x = null;");
        EXPECT_EQ(query("select k , x from t;", "b"), (rows_t{"(1, NULL)", "(2, 20)", "(3, 30)", "(4, NULL)"}));
        EXPECT_EQ(query("select k , x from t;"), (rows_t{"(1, 10)", "(2, NULL)", "(3, 30)"}));
        EXPECT_EQ(query("select k , x from t;", "c"), (rows_t{"(1, NULL)", "(2, NULL)", "(3, NULL)"}));
        EXPECT_EQ(query("select k from t version b where x is null;"), (rows_t{"(1)", "(4)"}));

        const rows_t expected = {"(b, 4, 2, 50, 20, 30, 25)", "(c, 3, 0, NULL, NULL, NULL, NULL)", "(master, 3, 2, 40, 10, 30, 20)"};
        const std::string aggregates = "select branch , count(*) , count(x) , sum(x) , min(x) , max(x) , avg(x) from t version all group by branch;";
        EXPECT_EQ(query(aggregates), expected);
        using Strategy = Algebra::Logical::BranchAggregation::Strategy;
        const Strategy strategy = Algebra::Logical::BranchAggregation::_strategy;
        Algebra::Logical::BranchAggregation::_strategy = (strategy == Strategy::Shared) ? Strategy::PerBranch : Strategy::Shared;
        EXPECT_EQ(query(aggregates), expected);
        Algebra::Logical::BranchAggregation::_strategy = strategy;
    }

    // MERGE and DIFF compare versions as IS NOT DISTINCT FROM: two NULLs are equal, NULL and a value not
    TEST_F(NullTest, MergeAndDiffCompareNulls) {
        exec("create branch b from main;");
        exec("update t version b set x = null where k = 1;");
        exec("update t version b set x = null where k = 2;"); // NULL already: no change
        exec("update t version b set s = 'c' where k = 3;");
        EXPECT_EQ(query("select * from diff ( t version main , t version b );"),
                  (rows_t{"(b, 1, NULL, a, 1.50)", "(b, 3, 30, c, 3.25)", "(master, 1, 10, a, 1.50)", "(master, 3, 30, NULL, 3.25)"}));
        EXPECT_EQ(query("select branch , k from diff ( t version main , t version b ) where x is null;"), (rows_t{"(b, 1)"}));
        EXPECT_EQ(query("select branch , count(*) from t version all where s is not null group by branch;"), (rows_t{"(b, 3)", "(master, 2)"}));

        // a conflict: both set x of k = 3, main to NULL and b to 33; KEEP SOURCE takes b's
        exec("update t set x = null where k = 3;");
        exec("update t version b set x = 33 where k = 3;");
        EXPECT_ANY_THROW(exec("merge branch b into main;"));
        exec("merge branch b into main on conflict keep source;");
        EXPECT_EQ(query("select k , x , s from t;"), (rows_t{"(1, NULL, a)", "(2, NULL, b)", "(3, 33, c)"}));
        EXPECT_EQ(query("select * from diff ( t version main , t version b );"), (rows_t{}));

        // both set the same attribute to NULL: no conflict
        exec("update t set x = null where k = 3;");
        exec("update t version b set x = null where k = 3;");
        exec("merge branch b into main;");
        EXPECT_EQ(query("select k , x from t where k = 3;"), (rows_t{"(3, NULL)"}));
    }

    // a column added to a branch without a default is NULL in the existing tuples; NOT NULL needs a default
    TEST_F(NullTest, AddedColumnsTakeNulls) {
        exec("create branch b from main;");
        exec("alter table t version b add column y integer;");
        exec("alter table t version b add column z integer null default 7;");
        EXPECT_THROW(exec("alter table t version b add column w integer not null;"), semanticalAnalysis::not_null_violation);
        EXPECT_THROW(exec("alter table t version b add column w integer not null default null;"), semanticalAnalysis::not_null_violation);
        exec("alter table t version b add column v integer default null;");
        exec("update t version b set y = k where k > 1;");
        exec("insert into t version b ( k , z ) values ( 4 , null );");
        EXPECT_EQ(query("select k , y , z , v from t;", "b"),
                  (rows_t{"(1, NULL, 7, NULL)", "(2, 2, 7, NULL)", "(3, 3, 7, NULL)", "(4, NULL, NULL, NULL)"}));
        EXPECT_EQ(query("select sum(y) , count(y) , count(z) from t version b;"), (rows_t{"(5, 2, 3)"}));
        exec("merge branch b into main;");
        EXPECT_EQ(query("select k , y , z from t;"), (rows_t{"(1, NULL, 7)", "(2, 2, 7)", "(3, 3, 7)", "(4, NULL, NULL)"}));
    }

    TEST_F(NullTest, CopyReadsAndWritesEmptyFieldsAsNull) {
        const std::string path = ::testing::internal::TempDir() + "tardisdb_null_test.csv";
        // COPY TO writes through its own callback, which execute() replaces by its sink
        QueryCompiler::compileAndExecute("copy t version master to '" + path + "' with format csv;", *db);
        exec("create table u ( k integer not null , x integer , s varchar ( 8 ) , d numeric ( 8 , 2 ) );");
        exec("copy u from '" + path + "' with format csv;");
        std::remove(path.c_str());
        EXPECT_EQ(query("select k , x , s , d from u;"), (rows_t{"(1, 10, a, 1.50)", "(2, NULL, b, NULL)", "(3, 30, NULL, 3.25)"}));
    }

    TEST_F(NullTest, ScansInParallel) {
        const size_t threads = Parallel::threads;
        Parallel::threads = 4;
        std::string values;
        for (int k = 4; k <= 3000; ++k) {
            values += std::string(values.empty() ? "" : " , ") + "( " + std::to_string(k) + " , " + (k % 3 == 0 ? "null" : std::to_string(k)) + " )";
        }
        exec("insert into t ( k , x ) values " + values + ";");
        auto rows = query("select count(*) , count(x) from t;");
        Parallel::threads = threads;
        // NULL: k = 2 and the 999 multiples of 3 from 6
        EXPECT_EQ(rows, (rows_t{"(3000, 2000)"}));
        EXPECT_EQ(query("select count(*) from t where x is null;"), (rows_t{"(1000)"}));
    }

}
