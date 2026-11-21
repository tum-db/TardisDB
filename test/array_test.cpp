// Arrays of PostgreSQL (any number of dimensions, lower bounds, NULL elements) and DOUBLE PRECISION:
// literals and their text, subscripts and slices, the functions and operators of arrays, ANY and ALL,
// expressions in the select list, WHERE and SET, branches with MERGE and DIFF, ALTER TABLE, COPY,
// computed arrays in parallel scans, the arithmetic, casts and aggregates of doubles, and both in joins

#include "codegen/LLVMCompat.hpp"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <mutex>
#include "foundations/Parallel.hpp"
#include "native/sql/SqlTuple.hpp"
#include "queryCompiler/queryCompiler.hpp"
#include "semanticAnalyser/SemanticAnalyser.hpp"
#include <gtest/gtest.h>

namespace {

    class ArrayTest : public ::testing::Test {
    protected:
        void SetUp() override {
            db = std::make_unique<Database>();
            exec("create table t ( k integer not null , a integer[] , s text[] , n numeric ( 6 , 2 ) [] );");
            exec("insert into t ( k , a , s , n ) values ( 1 , '{1,2,3}' , '{\"a b\",NULL,c}' , '{1.5,2}' ) , "
                 "( 2 , '[0:1]={7,NULL}' , '{}' , '{{1,2},{3,4}}' ) , ( 3 , null , null , null );");
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

        /// \returns The single value of a query
        std::string value(const std::string & sql) {
            auto rows = query(sql);
            if (rows.size() != 1) return "<" + std::to_string(rows.size()) + " rows>";
            return rows.front().substr(1, rows.front().size() - 2);
        }

        using rows_t = std::vector<std::string>;
        std::unique_ptr<Database> db;
    };

    TEST_F(ArrayTest, StoresAndPrintsArrays) {
        EXPECT_EQ(query("select k , a , s , n from t;"),
                  (rows_t{"(1, {1,2,3}, {\"a b\",NULL,c}, {1.50,2.00})", "(2, [0:1]={7,NULL}, {}, {{1.00,2.00},{3.00,4.00}})",
                          "(3, NULL, NULL, NULL)"}));
        // ARRAY[ ... ] of constants as a value, and the types of the elements
        exec("create table e ( k integer not null , b boolean[] , d date[] , ts timestamp[] , f double precision[] , v varchar ( 3 ) [] , c char ( 2 ) [] , l bigint array );");
        exec("insert into e values ( 1 , '{t,false,NULL}' , '{2025-01-02}' , ARRAY['2025-01-02 03:04:05'] , '{0.1,-2.5e-7,1e20,NaN,-Infinity}' , ARRAY['abc',NULL] , '{x}' , '{9000000000}' );");
        EXPECT_EQ(query("select b , d , ts , f , v , c , l from e;"),
                  (rows_t{"({t,f,NULL}, {2025-01-02}, {\"2025-01-02 03:04:05\"}, {0.1,-2.5e-07,1e+20,NaN,-Infinity}, {abc,NULL}, {\"x \"}, {9000000000})"}));
        // quoting: empty strings, NULL as a string, braces, quotes and backslashes
        exec("insert into t ( k , s ) values ( 4 , '{\"\",\"NULL\",\"{}\",\"a\\\"b\",\"c\\\\d\"}' );");
        EXPECT_EQ(value("select s from t where k = 4;"), "{\"\",\"NULL\",\"{}\",\"a\\\"b\",\"c\\\\d\"}");
        EXPECT_EQ(value("select array_to_string(s, '|') from t where k = 4;"), "|NULL|{}|a\"b|c\\d");
        // three dimensions and lower bounds of every dimension
        exec("insert into t ( k , a ) values ( 5 , '[2:3][0:0][-1:0]={{{1,2}},{{3,4}}}' );");
        EXPECT_EQ(value("select a from t where k = 5;"), "[2:3][0:0][-1:0]={{{1,2}},{{3,4}}}");
        EXPECT_EQ(value("select array_dims(a) from t where k = 5;"), "[2:3][0:0][-1:0]");
    }

    TEST_F(ArrayTest, RejectsMalformedArrays) {
        for (const char * literal : {"'{1,2'", "'{1,,2}'", "'{{1},{2,3}}'", "'{1,{2}}'", "'1,2'", "'{a}'", "'[1:3]={1,2}'", "'{1}x'"}) {
            EXPECT_THROW(exec(std::string("insert into t ( k , a ) values ( 9 , ") + literal + " );"), std::exception) << literal;
        }
        exec("create table v ( k integer not null , v varchar ( 2 ) [] );");
        EXPECT_THROW(exec("insert into v values ( 1 , '{abc}' );"), std::exception);
        EXPECT_EQ(query("select k from t where k = 9;"), rows_t{});
    }

    TEST_F(ArrayTest, SubscriptsAndSlices) {
        EXPECT_EQ(query("select k , a[1] , a[0] , a[4] , s[1] from t;"),
                  (rows_t{"(1, 1, NULL, NULL, a b)", "(2, NULL, 7, NULL, NULL)", "(3, NULL, NULL, NULL, NULL)"}));
        EXPECT_EQ(value("select n[2][1] from t where k = 2;"), "3.00");
        EXPECT_EQ(value("select n[2] from t where k = 2;"), "NULL"); // fewer subscripts than dimensions
        // slices start at 1; omitted bounds; a subscript among slices is a slice from 1
        EXPECT_EQ(value("select a[2:3] from t where k = 1;"), "{2,3}");
        EXPECT_EQ(value("select a[:2] from t where k = 1;"), "{1,2}");
        EXPECT_EQ(value("select a[2:] from t where k = 1;"), "{2,3}");
        EXPECT_EQ(value("select a[5:9] from t where k = 1;"), "{}");
        EXPECT_EQ(value("select n[1:2][2] from t where k = 2;"), "{{1.00,2.00},{3.00,4.00}}");
        EXPECT_EQ(value("select n[2:2][2:2] from t where k = 2;"), "{{4.00}}");
        EXPECT_EQ(value("select a[k] from t where k = 1;"), "1");
        EXPECT_EQ(value("select (ARRAY[10,20,30])[2] from t where k = 1;"), "20");
        EXPECT_EQ(query("select k from t where a[1] = 1 or a[0] = 7;"), (rows_t{"(1)", "(2)"}));
    }

    TEST_F(ArrayTest, FunctionsOfPostgreSQL) {
        auto one = [&](const std::string & expression) { return value("select " + expression + " from t where k = 1;"); };
        EXPECT_EQ(one("array_append(a, 4)"), "{1,2,3,4}");
        EXPECT_EQ(one("array_prepend(0, a)"), "{0,1,2,3}");
        EXPECT_EQ(one("array_cat(a, ARRAY[4,5])"), "{1,2,3,4,5}");
        EXPECT_EQ(one("a || 4"), "{1,2,3,4}");
        EXPECT_EQ(one("0 || a"), "{0,1,2,3}");
        EXPECT_EQ(one("a || '{4,5}'"), "{1,2,3,4,5}");
        EXPECT_EQ(one("ARRAY[1,2] || ARRAY[[3,4],[5,6]]"), "{{1,2},{3,4},{5,6}}");
        EXPECT_EQ(one("ARRAY[[1,2],[3,4]] || ARRAY[5,6]"), "{{1,2},{3,4},{5,6}}");
        EXPECT_EQ(one("array_length(a, 1)"), "3");
        EXPECT_EQ(one("array_length(a, 2)"), "NULL");
        EXPECT_EQ(one("array_lower(a, 1)"), "1");
        EXPECT_EQ(one("array_upper(a, 1)"), "3");
        EXPECT_EQ(one("array_ndims(ARRAY[[1,2],[3,4]])"), "2");
        EXPECT_EQ(one("cardinality(ARRAY[[1,2],[3,4]])"), "4");
        EXPECT_EQ(one("array_dims(ARRAY[[1,2,3],[4,5,6]])"), "[1:2][1:3]");
        EXPECT_EQ(one("array_position(ARRAY['A','B','A'], 'A', 2)"), "3");
        EXPECT_EQ(one("array_position(a, 4)"), "NULL");
        EXPECT_EQ(one("array_positions(ARRAY['A','A','B','A'], 'A')"), "{1,2,4}");
        EXPECT_EQ(one("array_position(ARRAY[1,NULL], NULL)"), "2");
        EXPECT_EQ(one("array_remove(ARRAY[1,2,NULL,2], 2)"), "{1,NULL}");
        EXPECT_EQ(one("array_remove(ARRAY[1,NULL], NULL)"), "{1}");
        EXPECT_EQ(one("array_replace(ARRAY[[1,2],[2,3]], 2, 9)"), "{{1,9},{9,3}}");
        EXPECT_EQ(one("array_fill(7, ARRAY[3], ARRAY[2])"), "[2:4]={7,7,7}");
        EXPECT_EQ(one("array_fill(0, ARRAY[2,2])"), "{{0,0},{0,0}}");
        EXPECT_EQ(one("array_to_string(ARRAY[1,2,3,NULL,5], ',', '*')"), "1,2,3,*,5");
        EXPECT_EQ(one("array_to_string(ARRAY[1,2,3,NULL,5], ',')"), "1,2,3,5");
        EXPECT_EQ(one("string_to_array('xx~^~yy~^~zz', '~^~', 'yy')"), "{xx,NULL,zz}");
        EXPECT_EQ(one("string_to_array('abc', NULL)"), "{a,b,c}");
        EXPECT_EQ(one("trim_array(ARRAY[1,2,3,4,5,6], 2)"), "{1,2,3,4}");
        // NULL arrays: append makes one, the others are NULL
        EXPECT_EQ(value("select array_append(a, 1) , cardinality(a) , a || ARRAY[2] from t where k = 3;"), "{1}, NULL, {2}");
        // the elements of a constant array take the type of the array they are appended to
        EXPECT_EQ(value("select n || 2.5 from t where k = 1;"), "{1.50,2.00,2.50}");
        // errors of the runtime end the statement
        EXPECT_THROW(query("select array_append(n, 1.0) from t where k = 2;"), std::exception);
        EXPECT_THROW(query("select array_fill(1, ARRAY[-1]) from t where k = 1;"), std::exception);
        EXPECT_THROW(query("select nosuchfunction(a) from t;"), semanticalAnalysis::semantic_sql_error);
        EXPECT_THROW(query("select k from t where a = '{1,x}';"), std::exception);
    }

    TEST_F(ArrayTest, OperatorsAnyAndAll) {
        EXPECT_EQ(query("select k from t where a @> '{3,1,3}';"), (rows_t{"(1)"}));
        EXPECT_EQ(query("select k from t where a <@ ARRAY[1,2,3,4];"), (rows_t{"(1)"}));
        EXPECT_EQ(query("select k from t where a && '{7,9}';"), (rows_t{"(2)"}));
        EXPECT_EQ(query("select k from t where 3 = any(a);"), (rows_t{"(1)"}));
        EXPECT_EQ(query("select k from t where 9 <> all(a);"), (rows_t{"(1)"})); // {7,NULL}: NULL
        EXPECT_EQ(query("select k from t where 2 < any(a) and k < 5;"), (rows_t{"(1)", "(2)"}));
        EXPECT_EQ(query("select k from t where k = any('{1,3}');"), (rows_t{"(1)", "(3)"}));
        // equality and order: elements, then the number of elements, then the shape; NULL elements equal each other
        EXPECT_EQ(query("select k from t where a = '{1,2,3}';"), (rows_t{"(1)"}));
        EXPECT_EQ(query("select k from t where a = ARRAY[7,NULL];"), rows_t{}); // the lower bounds differ
        EXPECT_EQ(query("select k from t where a = '[0:1]={7,NULL}';"), (rows_t{"(2)"}));
        EXPECT_EQ(query("select k from t where a < '{1,2,4}';"), (rows_t{"(1)"}));
        EXPECT_EQ(query("select k from t where a > '{1,2}' and a <= '{1,2,3}';"), (rows_t{"(1)"}));
        EXPECT_EQ(query("select k from t where a <> '{1,2,3}';"), (rows_t{"(2)"}));
        EXPECT_EQ(query("select k from t where a is null;"), (rows_t{"(3)"}));
        EXPECT_EQ(query("select k from t where cardinality(s) = 0;"), (rows_t{"(2)"}));
    }

    TEST_F(ArrayTest, ExpressionsOfTheSelectListAndWhere) {
        EXPECT_EQ(query("select k * 2 + 1 as x , a[1] + k first from t where k between 1 and 2;"), (rows_t{"(3, 2)", "(5, NULL)"}));
        EXPECT_EQ(query("select k from t where not ( k = 1 or k = 2 );"), (rows_t{"(3)"}));
        EXPECT_EQ(query("select k from t where k not in ( 1 , 3 );"), (rows_t{"(2)"}));
        EXPECT_EQ(query("select k from t where k * k > 3 and k - 1 <> 2;"), (rows_t{"(2)"}));
        EXPECT_EQ(query("select k , - k from t where k = 2;"), (rows_t{"(2, -2)"}));
        // the names of computed columns (as PostgreSQL), and aliases
        std::vector<std::string> names;
        QueryCompiler::execute("select k , a[1] , cardinality(a) , ARRAY[k] , k + 1 , k + 2 as two from t;", *db, "main",
                               [](Native::Sql::SqlTuple *) { },
                               [&](const QueryCompiler::StatementResult & result) { names = result.columnNames; });
        EXPECT_EQ(names, (std::vector<std::string>{"k", "a", "cardinality", "array", "?column?", "two"}));
        // queries over several branches and DIFF take any condition, but columns (and aggregates of them) only
        EXPECT_EQ(query("select branch , count(*) from t version all where a[1] = 1 or k > 2 group by branch;"), (rows_t{"(master, 2)"}));
        EXPECT_EQ(query("select count(*) from t where k between 2 and 3 and k <> 3;"), (rows_t{"(1)"}));
        EXPECT_THROW(query("select sum(a[1]) from t;"), std::exception);
        EXPECT_THROW(query("select a[1] from diff ( t version main , t version main );"), std::exception);
    }

    TEST_F(ArrayTest, UpdatesBranchesMergeAndDiff) {
        exec("update t set a = array_append(a, 4) , s = s || 'd' where k = 1;");
        EXPECT_EQ(value("select a , s from t where k = 1;"), "{1,2,3,4}, {\"a b\",NULL,c,d}");
        exec("update t set a = ARRAY[k, k * 10] where k = 3;");
        EXPECT_EQ(value("select a from t where k = 3;"), "{3,30}");
        exec("create branch b from main;");
        exec("update t version b set a = a[2:3] where k = 1;");
        exec("update t version b set n = NULL where k = 2;");
        // the versions keep their arrays
        EXPECT_EQ(query("select k , a from t version b where k <> 3;"), (rows_t{"(1, {2,3})", "(2, [0:1]={7,NULL})"}));
        EXPECT_EQ(query("select k , a from t where k <> 3;"), (rows_t{"(1, {1,2,3,4})", "(2, [0:1]={7,NULL})"}));
        EXPECT_EQ(query("select branch , k , a , n from diff ( t version main , t version b );"),
                  (rows_t{"(b, 1, {2,3}, {1.50,2.00})", "(b, 2, [0:1]={7,NULL}, NULL)",
                          "(master, 1, {1,2,3,4}, {1.50,2.00})", "(master, 2, [0:1]={7,NULL}, {{1.00,2.00},{3.00,4.00}})"}));
        // the same array in both is no conflict; different ones are
        exec("update t set a = '{2,3}' where k = 1;");
        exec("merge branch b into main;");
        EXPECT_EQ(query("select k , a , n from t where k < 3;"), (rows_t{"(1, {2,3}, {1.50,2.00})", "(2, [0:1]={7,NULL}, NULL)"}));
        // a column that is NOT NULL takes no array that may be NULL
        exec("create table w ( k integer not null , a integer[] not null );");
        exec("insert into w values ( 1 , '{1}' );");
        EXPECT_THROW(exec("update w set a = a[1:1] || a[5] where k = 1;"), semanticalAnalysis::not_null_violation);
        EXPECT_THROW(exec("update w set a = '{x}' where k = 1;"), std::exception);
        EXPECT_THROW(exec("update w set a = 5 where k = 1;"), std::exception);
    }

    TEST_F(ArrayTest, AlterTableAndCopy) {
        exec("alter table t add column z bigint[] default '{1,2}';");
        exec("alter table t add column y text[];");
        EXPECT_EQ(query("select k , z , y from t;"), (rows_t{"(1, {1,2}, NULL)", "(2, {1,2}, NULL)", "(3, {1,2}, NULL)"}));
        exec("update t set z = z || 3 , y = ARRAY['p'] where k = 1;");
        EXPECT_EQ(value("select z , y from t where k = 1;"), "{1,2,3}, {p}");

        const std::string path = ::testing::internal::TempDir() + "tardisdb_array_copy.csv";
        std::remove(path.c_str());
        QueryCompiler::compileAndExecute("copy t to '" + path + "' with format csv;", *db);
        exec("create table c ( k integer not null , a integer[] , s text[] , n numeric ( 6 , 2 ) [] , z bigint[] , y text[] );");
        exec("copy c from '" + path + "' with format csv;");
        EXPECT_EQ(query("select k , a , s , n , z , y from c;"), query("select k , a , s , n , z , y from t;"));
        std::remove(path.c_str());
    }

    // computed arrays of the workers of a parallel scan live in the arena of the query
    TEST_F(ArrayTest, ComputesArraysInParallel) {
        const size_t threads = Parallel::threads;
        Parallel::threads = 8;
        std::string values;
        for (int k = 10; k < 4010; ++k) {
            values += std::string(values.empty() ? "" : " , ") + "( " + std::to_string(k) + " , '{" + std::to_string(k) + "," +
                      std::to_string(k % 7) + "}' )";
        }
        exec("insert into t ( k , a ) values " + values + ";");
        auto rows = query("select k , array_append(a, k) , array_to_string(a || a, '-') from t where a[2] = 3 and k >= 10;");
        auto sums = query("select count(*) , sum(k) from t where 3 = any(a) and k >= 10;");
        Parallel::threads = threads;
        ASSERT_EQ(rows.size(), 572u); // k % 7 = 3 for 572 of 4000
        EXPECT_EQ(rows.front(), "(10, {10,3,10}, 10-3-10-3)");
        EXPECT_EQ(sums, (rows_t{"(572, 1148862)"}));
    }

    TEST_F(ArrayTest, DoublePrecision) {
        exec("create table f ( k integer not null , d double precision , r real , x float8 , n numeric ( 6 , 2 ) );");
        exec("insert into f values ( 1 , 1.5 , 0.25 , 1e300 , 1.25 ) , ( 2 , -2.25 , NULL , 'NaN' , 2.5 ) , ( 3 , NULL , 3 , '-Infinity' , NULL ) , ( 4 , 0.1 , 4 , 0 , 3 );");
        EXPECT_EQ(query("select k , d , x from f;"), (rows_t{"(1, 1.5, 1e+300)", "(2, -2.25, NaN)", "(3, NULL, -Infinity)", "(4, 0.1, 0)"}));
        // arithmetic with the other numbers, casts (half to even, as PostgreSQL)
        EXPECT_EQ(query("select k , d * 2 , d + k , n * d , d / 4 from f where d is not null;"),
                  (rows_t{"(1, 3, 2.5, 1.875, 0.375)", "(2, -4.5, -0.25, -5.625, -0.5625)", "(4, 0.2, 4.1, 0.30000000000000004, 0.025)"}));
        EXPECT_EQ(query("select k , d::integer , cast(d as numeric(6,1)) , k::double precision / 8 from f where d is not null;"),
                  (rows_t{"(1, 2, 1.5, 0.125)", "(2, -2, -2.3, 0.25)", "(4, 0, 0.1, 0.5)"}));
        // comparisons: NaN equals NaN and is greater than every other double
        EXPECT_EQ(query("select k from f where x > 1e299;"), (rows_t{"(1)", "(2)"}));
        EXPECT_EQ(query("select k from f where x = 'NaN';"), (rows_t{"(2)"}));
        EXPECT_EQ(query("select k from f where d between -3 and 0.1;"), (rows_t{"(2)", "(4)"}));
        // aggregates skip NULLs; sums of doubles are computed per branch (the shared scan would cancel them)
        using Strategy = Algebra::Logical::BranchAggregation::Strategy;
        for (Strategy strategy : {Strategy::Shared, Strategy::PerBranch}) {
            Algebra::Logical::BranchAggregation::_strategy = strategy;
            EXPECT_EQ(query("select count(d) , sum(d) , avg(d) , min(d) , max(d) , max(x) , min(x) from f;"),
                      (rows_t{"(3, -0.65, -0.21666666666666667, -2.25, 1.5, NaN, -Infinity)"}));
        }
        Algebra::Logical::BranchAggregation::_strategy = Strategy::Shared;
        exec("create branch fb from main;");
        exec("update f version fb set d = 1e20 where k = 1;");
        exec("insert into f version fb values ( 5 , 1 , 1 , 1 , 1 );");
        EXPECT_EQ(query("select branch , sum(d) , count(*) from f version all group by branch;"),
                  (rows_t{"(fb, 1e+20, 5)", "(master, -0.65, 4)"}));
        EXPECT_THROW(exec("insert into f ( k , d ) values ( 9 , 'abc' );"), std::exception);
    }

    TEST_F(ArrayTest, JoinsCarryArraysAndDoubles) {
        exec("create table j ( k integer not null , w double precision , b integer[] );");
        exec("insert into j values ( 1 , 0.5 , '{10,20}' ) , ( 2 , 2.5 , null ) , ( 2 , -1 , '{30}' ) , ( 9 , 1 , '{}' );");
        // arrays and doubles of both sides, in the hash table and in expressions after the join
        EXPECT_EQ(query("select t.k , t.a , j.b , j.w from t , j where t.k = j.k;"),
                  (rows_t{"(1, {1,2,3}, {10,20}, 0.5)", "(2, [0:1]={7,NULL}, NULL, 2.5)", "(2, [0:1]={7,NULL}, {30}, -1)"}));
        EXPECT_EQ(query("select j.k , t.a || j.b , j.w * 2 from j , t where j.k = t.k and cardinality(j.b) > 0;"),
                  (rows_t{"(1, {1,2,3,10,20}, 1)", "(2, [0:2]={7,NULL,30}, -2)"}));
        // arrays and doubles as join keys
        exec("create table ja ( b integer[] , w double precision , x integer not null );");
        exec("insert into ja values ( '{10,20}' , 2.5 , 1 ) , ( '{30}' , 0.5 , 2 ) , ( null , null , 3 );");
        EXPECT_EQ(query("select j.k , ja.x from j , ja where j.b = ja.b;"), (rows_t{"(1, 1)", "(2, 2)"}));
        EXPECT_EQ(query("select j.k , ja.x from j , ja where j.w = ja.w;"), (rows_t{"(1, 2)", "(2, 1)"}));
    }

}
