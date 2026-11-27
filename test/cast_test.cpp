// The casts of sql/SqlCasts.hpp: contexts, common types, implicit, explicit and assignment casts (also to strings), joins, run-time errors

#include "codegen/LLVMCompat.hpp"
#include <algorithm>
#include <mutex>
#include "foundations/Parallel.hpp"
#include "runtime/SqlTuple.hpp"
#include "queryCompiler/queryCompiler.hpp"
#include "sql/SqlCasts.hpp"
#include <gtest/gtest.h>

namespace {

    using rows_t = std::vector<std::string>;
    using Sql::Casts::Context;

    class CastTest : public ::testing::Test {
    protected:
        void SetUp() override {
            db = std::make_unique<Database>();
            exec("CREATE TABLE ci ( id integer not null, n numeric(10, 2), v varchar(20), c char(4), c1 char(1), d double precision );");
            exec("CREATE TABLE cb ( id bigint, w varchar(24), t text, c char(3), d double precision );");
            exec("CREATE TABLE ce ( id integer not null, v varchar(10) );");
            exec("CREATE TABLE cn ( id integer, v varchar(10) );");
            exec("INSERT INTO ci VALUES (1, 1.50, 'one', 'ab', 'a', 1.5), (2, 2.00, 'two', 'b', 'b', 2.0), "
                 "(3, NULL, 'a long varchar key 3', NULL, NULL, 3.0), (4, -2.50, NULL, 'cd', 'c', NULL);");
            exec("INSERT INTO cb VALUES (1, 'one', 'one', 'ab ', 1.5), (2, 'deux', 'two', 'b', 2.25), "
                 "(3, 'a long varchar key 3', 'a long varchar key 3', 'x', NULL), (NULL, NULL, NULL, 'cd', 4.0), (5000000000, 'big', 'a text longer than 15 bytes', NULL, NULL);");
            exec("INSERT INTO cn VALUES (1, 'one');");
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

        /// \returns The rows of a join with the hash join, sorted, which the merge join gives as well
        rows_t join(const std::string & sql) {
            const rows_t hashed = run(sql);
            QueryCompiler::setSetting("enable_hashjoin", "off");
            EXPECT_EQ(hashed, run(sql)) << sql;
            QueryCompiler::setSetting("enable_hashjoin", "on");
            return hashed;
        }

        std::unique_ptr<Database> db;
    };

    TEST_F(CastTest, Table) {
        // widening casts happen anywhere, narrowing ones on an assignment, the others only when written
        EXPECT_TRUE(Sql::Casts::canCast(Sql::getIntegerTy(), Sql::getLongIntegerTy(), Context::Implicit));
        EXPECT_TRUE(Sql::Casts::canCast(Sql::getLongIntegerTy(), Sql::getDoubleTy(), Context::Implicit));
        EXPECT_FALSE(Sql::Casts::canCast(Sql::getLongIntegerTy(), Sql::getIntegerTy(), Context::Implicit));
        EXPECT_TRUE(Sql::Casts::canCast(Sql::getLongIntegerTy(), Sql::getIntegerTy(), Context::Assignment));
        EXPECT_FALSE(Sql::Casts::canCast(Sql::getDoubleTy(), Sql::getNumericTy(10, 2), Context::Implicit));
        EXPECT_TRUE(Sql::Casts::canCast(Sql::getNumericTy(10, 2), Sql::getNumericTy(12, 4), Context::Implicit));
        EXPECT_FALSE(Sql::Casts::canCast(Sql::getNumericTy(12, 4), Sql::getNumericTy(10, 2), Context::Implicit));
        EXPECT_TRUE(Sql::Casts::canCast(Sql::getNumericTy(12, 4), Sql::getNumericTy(10, 2), Context::Assignment));
        EXPECT_TRUE(Sql::Casts::canCast(Sql::getVarcharTy(10), Sql::getVarcharTy(12), Context::Implicit));
        EXPECT_FALSE(Sql::Casts::canCast(Sql::getVarcharTy(12), Sql::getVarcharTy(10), Context::Implicit));
        EXPECT_TRUE(Sql::Casts::canCast(Sql::getCharTy(4), Sql::getTextTy(), Context::Implicit));
        EXPECT_FALSE(Sql::Casts::canCast(Sql::getTextTy(), Sql::getVarcharTy(5), Context::Implicit));
        EXPECT_TRUE(Sql::Casts::canCast(Sql::getTextTy(), Sql::getVarcharTy(5), Context::Explicit));
        EXPECT_FALSE(Sql::Casts::canCast(Sql::getTextTy(), Sql::getIntegerTy(), Context::Explicit));
        EXPECT_FALSE(Sql::Casts::canCast(Sql::getDateTy(), Sql::getIntegerTy(), Context::Explicit));
        EXPECT_FALSE(Sql::Casts::canCast(Sql::getIntegerTy(), Sql::getTextTy(), Context::Implicit));
        EXPECT_TRUE(Sql::Casts::canCast(Sql::getIntegerTy(), Sql::getTextTy(), Context::Assignment));
        EXPECT_TRUE(Sql::Casts::canCast(Sql::getDateTy(), Sql::getVarcharTy(10), Context::Explicit));
        EXPECT_TRUE(Sql::Casts::canCast(Sql::getDateTy(), Sql::getDateTy(true), Context::Implicit));

        auto common = [](Sql::SqlType a, Sql::SqlType b) {
            Sql::SqlType result;
            return Sql::Casts::commonType(a, b, result) ? Sql::Casts::typeName(result) : std::string("none");
        };
        EXPECT_EQ(common(Sql::getIntegerTy(), Sql::getLongIntegerTy(true)), "bigint");
        EXPECT_EQ(common(Sql::getLongIntegerTy(), Sql::getNumericTy(10, 2)), "numeric(10,2)");
        EXPECT_EQ(common(Sql::getNumericTy(10, 2), Sql::getNumericTy(6, 4)), "numeric(12,4)");
        EXPECT_EQ(common(Sql::getNumericTy(10, 2), Sql::getDoubleTy()), "double precision");
        EXPECT_EQ(common(Sql::getVarcharTy(10), Sql::getVarcharTy(12)), "character varying(12)");
        EXPECT_EQ(common(Sql::getCharTy(4), Sql::getCharTy(3)), "character varying(4)");
        EXPECT_EQ(common(Sql::getCharTy(4), Sql::getTextTy()), "text");
        EXPECT_EQ(common(Sql::getTextTy(), Sql::getIntegerTy()), "none");
        EXPECT_EQ(common(Sql::getBoolTy(), Sql::getBoolTy(true)), "boolean");
    }

    TEST_F(CastTest, Joins) {
        // integer = bigint, varchar(20) = varchar(24), varchar = text, char = char of another length (without
        // trailing spaces), numeric = double precision, numeric = bigint; NULL keys join nothing
        EXPECT_EQ(join("SELECT ci.id, cb.id FROM ci, cb WHERE ci.id = cb.id;"), (rows_t{"(1, 1)", "(2, 2)", "(3, 3)"}));
        EXPECT_EQ(join("SELECT ci.v, cb.w FROM ci, cb WHERE ci.v = cb.w;"), (rows_t{"(a long varchar key 3, a long varchar key 3)", "(one, one)"}));
        EXPECT_EQ(join("SELECT ci.v, cb.t FROM ci, cb WHERE ci.v = cb.t;"), (rows_t{"(a long varchar key 3, a long varchar key 3)", "(one, one)", "(two, two)"}));
        EXPECT_EQ(join("SELECT ci.id, cb.c FROM ci, cb WHERE ci.c = cb.c;"), (rows_t{"(1, ab )", "(2, b)", "(4, cd)"}));
        EXPECT_EQ(join("SELECT ci.id, cb.d FROM ci, cb WHERE ci.n = cb.d;"), (rows_t{"(1, 1.5)"}));
        EXPECT_EQ(join("SELECT ci.id, cb.id FROM ci, cb WHERE ci.n = cb.id;"), (rows_t{"(2, 2)"}));
        EXPECT_EQ(join("SELECT ci.id, cb.id FROM ci, cb WHERE ci.d = cb.id;"), (rows_t{"(2, 2)", "(3, 3)"}));
        // the condition written the other way round, and with a second key
        EXPECT_EQ(join("SELECT ci.id FROM ci, cb WHERE cb.id = ci.id AND cb.t = ci.v;"), (rows_t{"(1)", "(2)", "(3)"}));
    }

    TEST_F(CastTest, Comparisons) {
        EXPECT_EQ(run("SELECT id FROM cb WHERE id = 2;"), (rows_t{"(2)"}));
        EXPECT_EQ(run("SELECT id FROM cb WHERE id > 3000000000;"), (rows_t{"(5000000000)"}));
        EXPECT_EQ(run("SELECT id FROM ci WHERE id < 2.5;"), (rows_t{"(1)", "(2)"}));
        EXPECT_EQ(run("SELECT id FROM ci WHERE n = d;"), (rows_t{"(1)", "(2)"}));
        EXPECT_EQ(run("SELECT id FROM ci WHERE c1 = c;"), (rows_t{"(2)"}));
        EXPECT_EQ(run("SELECT id FROM ci WHERE v < c;"), (rows_t{}));
        EXPECT_EQ(run("SELECT id FROM ci WHERE v > c1;"), (rows_t{"(1)", "(2)"}));
        EXPECT_EQ(run("SELECT id FROM ci WHERE id IN (1, 3000000000);"), (rows_t{"(1)"}));
        EXPECT_THROW(exec("SELECT id FROM cb WHERE t = id;"), std::exception);
    }

    TEST_F(CastTest, Arithmetic) {
        // bigint with bigint, with an integer, a numeric and a double
        EXPECT_EQ(run("SELECT id + 0, id * 2, id - 1, id / 2 FROM cb WHERE id < 3;"), (rows_t{"(1, 2, 0, 0)", "(2, 4, 1, 1)"}));
        EXPECT_EQ(run("SELECT cb.id + ci.id, ci.id * cb.id FROM ci, cb WHERE ci.id = cb.id AND ci.id > 1;"), (rows_t{"(4, 4)", "(6, 9)"}));
        EXPECT_EQ(run("SELECT id + 0.25, id * 1.5 FROM cb WHERE id = 2;"), (rows_t{"(2.25, 3.00)"}));
        EXPECT_EQ(run("SELECT id + d FROM cb WHERE id = 2;"), (rows_t{"(4.25)"}));
        EXPECT_EQ(run("SELECT id + 1 FROM cb WHERE id > 3;"), (rows_t{"(5000000001)"}));
        EXPECT_EQ(run("SELECT 0 - id FROM cb WHERE id = 3;"), (rows_t{"(-3)"}));
    }

    TEST_F(CastTest, Explicit) {
        // strings: truncated to the length, char without its trailing spaces; numbers rounded half away from zero
        EXPECT_EQ(run("SELECT CAST(t AS varchar(5)) FROM cb WHERE id > 2;"), (rows_t{"(a lon)", "(a tex)"}));
        EXPECT_EQ(run("SELECT CAST(v AS text) FROM ci WHERE id = 3;"), (rows_t{"(a long varchar key 3)"}));
        EXPECT_EQ(run("SELECT CAST(c AS varchar(4)), CAST(c AS text) FROM cb WHERE id = 1;"), (rows_t{"(ab, ab)"}));
        EXPECT_EQ(run("SELECT CAST(w AS char(2)) FROM cb WHERE id = 2;"), (rows_t{"(de)"}));
        EXPECT_EQ(run("SELECT CAST(n AS integer), CAST(n AS bigint), CAST(n AS numeric(10, 1)) FROM ci WHERE n IS NOT NULL;"),
                  (rows_t{"(-3, -3, -2.5)", "(2, 2, 1.5)", "(2, 2, 2.0)"}));
        EXPECT_EQ(run("SELECT CAST(id AS integer), CAST(d AS bigint) FROM cb WHERE id = 2;"), (rows_t{"(2, 2)"}));
        EXPECT_EQ(run("SELECT v::text FROM ci WHERE id = 1;"), (rows_t{"(one)"}));
        EXPECT_THROW(exec("SELECT CAST(t AS integer) FROM cb;"), std::exception);
        // to strings as PostgreSQL writes the values, truncated to the length; NULL stays NULL
        EXPECT_EQ(run("SELECT CAST(id AS text), CAST(n AS text), CAST(d AS varchar(3)), CAST(id > 1 AS text), CAST(id * 1000000000000000 AS text) "
                      "FROM ci WHERE id < 3;"), (rows_t{"(1, 1.50, 1.5, false, 1000000000000000)", "(2, 2.00, 2, true, 2000000000000000)"}));
        EXPECT_EQ(run("SELECT CAST(n AS text) FROM ci WHERE n IS NULL;"), (rows_t{"(NULL)"}));
        EXPECT_EQ(run("SELECT CAST(1::int AS text), CAST('{1,2}'::int[] AS text), CAST('2026-11-27'::date AS char(10));"),
                  (rows_t{"(1, {1,2}, 2026-11-27)"}));
    }

    TEST_F(CastTest, Assignment) {
        // SET c = ...: narrowing casts (rounded), strings that fit
        exec("UPDATE ci SET n = id + 1 WHERE id = 2;");
        exec("UPDATE ci SET id = id * 1.5 WHERE id = 1;");
        exec("UPDATE ci SET id = id * 2.5 WHERE id = 3;");
        exec("UPDATE ci SET v = c WHERE id = 4;");
        EXPECT_EQ(run("SELECT id, n, v FROM ci;"),
                  (rows_t{"(2, 1.50, one)", "(2, 3.00, two)", "(4, -2.50, cd)", "(8, NULL, a long varchar key 3)"}));
        // a decimal constant, rounded to the column
        exec("UPDATE ci SET id = 6.5, n = 1.555 WHERE id = 4;");
        EXPECT_EQ(run("SELECT id, n FROM ci WHERE v = 'cd';"), (rows_t{"(7, 1.56)"}));
        EXPECT_THROW(exec("UPDATE ci SET id = v;"), std::exception);
        EXPECT_THROW(exec("UPDATE ci SET n = c;"), std::exception);
        // numbers into strings
        exec("UPDATE ci SET v = n WHERE id = 7;");
        EXPECT_EQ(run("SELECT v FROM ci WHERE id = 7;"), (rows_t{"(1.56)"}));
    }

    TEST_F(CastTest, RuntimeErrors) {
        // an overflow, a division by zero and a string too long end the statement with the error of PostgreSQL
        auto error = [&](const std::string & sql) {
            try {
                exec(sql);
            } catch (const std::exception & e) {
                return std::string(e.what());
            }
            return std::string("no error");
        };
        EXPECT_EQ(error("SELECT id + 2147483647 FROM ci;"), "integer out of range");
        EXPECT_EQ(error("SELECT CAST(id AS bigint) * 4000000000000000000 FROM ci;"), "bigint out of range");
        EXPECT_EQ(error("SELECT CAST(CAST(id AS bigint) * 1000000000 AS integer) FROM ci;"), "integer out of range");
        EXPECT_EQ(error("SELECT CAST(d * 1e300 AS numeric(10, 2)) FROM ci;"), "numeric field overflow");
        EXPECT_EQ(error("SELECT id / (id - id) FROM ci;"), "division by zero");
        EXPECT_EQ(error("SELECT n / (n - n) FROM ci WHERE n IS NOT NULL;"), "division by zero");
        EXPECT_EQ(error("SELECT d / (d - d) FROM ci WHERE d IS NOT NULL;"), "division by zero");
        // writes: the table as it was
        EXPECT_EQ(error("UPDATE ci SET c = v WHERE id = 3;"), "value too long for type character(4)");
        EXPECT_EQ(run("SELECT c FROM ci WHERE id = 3;"), (rows_t{"(NULL)"}));
        EXPECT_EQ(error("INSERT INTO ce SELECT 2147483646 + id, v FROM ci;"), "integer out of range");
        EXPECT_EQ(run("SELECT count(*) FROM ce;"), (rows_t{"(0)"}));
        // in the workers of a parallel scan
        const size_t threads = Parallel::threads, morselSize = Parallel::morselSize;
        Parallel::threads = 4;
        Parallel::morselSize = 1;
        EXPECT_EQ(error("SELECT id + 2147483646 FROM ci;"), "integer out of range");
        Parallel::threads = threads;
        Parallel::morselSize = morselSize;
        EXPECT_EQ(run("SELECT id FROM ci WHERE id = 1;"), (rows_t{"(1)"}));
    }

    TEST_F(CastTest, EmptyBuildSide) {
        // a hash join whose left side is a scan of an empty table
        EXPECT_EQ(join("SELECT ce.id FROM ce, ci WHERE ce.id = ci.id;"), rows_t{});
        EXPECT_EQ(join("SELECT ce.v FROM ce, cn WHERE ce.v = cn.v;"), rows_t{});
        EXPECT_EQ(join("SELECT ce.id FROM ce, cb WHERE ce.id = cb.id;"), rows_t{});
        EXPECT_EQ(join("SELECT ce.id FROM ci, ce WHERE ci.id = ce.id;"), rows_t{});
    }

} // namespace
