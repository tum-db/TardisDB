// COPY: PostgreSQL's CSV (quotes, line breaks in fields, HEADER, DELIMITER, NULL, QUOTE, ESCAPE), the tbl
// format of the TPC-H tools, both forms of the options, loading into a branch, errors that leave the table
// as it was, and COPY ... TO through the statements of a session

#include "codegen/LLVMCompat.hpp"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include "runtime/SqlTuple.hpp"
#include "queryCompiler/queryCompiler.hpp"
#include "semanticAnalyser/SemanticAnalyser.hpp"
#include "sqlParser/SQLParser.hpp"
#include <gtest/gtest.h>

namespace {

    class CopyTest : public ::testing::Test {
    protected:
        void SetUp() override {
            db = std::make_unique<Database>();
            exec("create table c ( k integer not null , s varchar ( 16 ) , t text , x integer );");
            path = ::testing::internal::TempDir() + "tardisdb_copy_test_" +
                   ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".txt";
        }

        void TearDown() override { std::remove(path.c_str()); }

        QueryCompiler::StatementResult exec(const std::string & sql, const std::string & branch = "main") {
            return QueryCompiler::execute(sql, *db, branch, [](Native::Sql::SqlTuple *) { });
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

        void write(const std::string & content) {
            std::ofstream out(path, std::ios::binary);
            out << content;
        }

        std::string read() {
            std::ifstream in(path, std::ios::binary);
            std::stringstream content;
            content << in.rdbuf();
            return content.str();
        }

        /// \returns The message of the error of the statement ("" without one)
        std::string error(const std::string & sql) {
            try {
                exec(sql);
            } catch (const std::exception & e) {
                return e.what();
            }
            return "";
        }

        using rows_t = std::vector<std::string>;
        std::unique_ptr<Database> db;
        std::string path;
    };

    const std::string csvFile =
            "k,s,t,x\n"
            "1,plain,\"has, comma\",10\n"
            "2,\"say \"\"hi\"\"\",\"two\n"
            "lines\",\n"
            "3,\"\",,30\n";

    // a quoted field may hold the delimiter, quotes and line breaks; an unquoted empty field is NULL, a quoted one
    // the empty string
    TEST_F(CopyTest, ReadsCsv) {
        write(csvFile);
        auto result = exec("copy c from '" + path + "' with ( format csv , header );");
        EXPECT_EQ(result.tag, "COPY");
        EXPECT_EQ(result.rows, 3u);
        EXPECT_EQ(query("select t from c where k = 1;"), (rows_t{"(has, comma)"}));
        EXPECT_EQ(query("select s from c where k = 2;"), (rows_t{"(say \"hi\")"}));
        EXPECT_EQ(query("select t from c where k = 2;"), (rows_t{"(two\nlines)"}));
        EXPECT_EQ(query("select k from c where s is null;"), (rows_t{}));
        EXPECT_EQ(query("select k from c where s = '';"), (rows_t{"(3)"}));
        EXPECT_EQ(query("select k from c where t is null;"), (rows_t{"(3)"}));
        EXPECT_EQ(query("select k from c where x is null;"), (rows_t{"(2)"}));
        EXPECT_EQ(query("select count(*) from c;"), (rows_t{"(3)"}));
    }

    // COPY TO quotes what would not read back as it was: the delimiter, quotes, line breaks and the empty string
    TEST_F(CopyTest, WritesCsvThatReadsBack) {
        write(csvFile);
        exec("copy c from '" + path + "' csv header;");
        auto result = exec("copy c to '" + path + "' with ( format csv , header true );");
        EXPECT_EQ(result.tag, "COPY");
        EXPECT_EQ(result.rows, 3u);
        EXPECT_FALSE(result.returnsRows);
        EXPECT_EQ(read(), csvFile);
        exec("create table d ( k integer not null , s varchar ( 16 ) , t text , x integer );");
        exec("copy d from '" + path + "' with ( format csv , header );");
        EXPECT_EQ(query("select k , s , t , x from d;"), query("select k , s , t , x from c;"));
        EXPECT_EQ(query("select k from d where t is null;"), (rows_t{"(3)"}));
    }

    TEST_F(CopyTest, TakesTheOptionsInBothForms) {
        write("k;s;t;x\n1;a;NA;NA\n2;\"NA\";b;5\n");
        exec("copy c from '" + path + "' with format csv header delimiter ';' null 'NA';");
        exec("copy c from '" + path + "' csv header delimiter as ';' null as 'NA';");
        exec("copy c from '" + path + "' ( format csv , header on , delimiter ';' , null 'NA' );");
        EXPECT_EQ(query("select count(*) , count(t) , count(x) from c;"), (rows_t{"(6, 3, 3)"}));
        EXPECT_EQ(query("select count(*) from c where s = 'NA';"), (rows_t{"(3)"})); // quoted: not NULL

        write("7,'it''s',x,1\n");
        exec("copy c from '" + path + "' with ( format csv , quote '''' );");
        EXPECT_EQ(query("select s from c where k = 7;"), (rows_t{"(it's)"}));

        write("8,\"a \\\" b\",y,1\n");
        exec("copy c from '" + path + "' with ( format csv , escape '\\' );");
        EXPECT_EQ(query("select s from c where k = 8;"), (rows_t{"(a \" b)"}));
    }

    // the format of the TPC-H tools: '|' without quotes, a line may end with the delimiter
    TEST_F(CopyTest, ReadsAndWritesTbl) {
        write("1|a|b|10|\n2|x|y||\n");
        EXPECT_EQ(exec("copy c from '" + path + "' with format tbl;").rows, 2u);
        EXPECT_EQ(query("select k , s , t , x from c;"), (rows_t{"(1, a, b, 10)", "(2, x, y, NULL)"}));
        exec("copy c to '" + path + "' ( format tbl );");
        EXPECT_EQ(read(), "1|a|b|10\n2|x|y|\n");
        exec("insert into c ( k , s ) values ( 3 , 'p|q' );");
        EXPECT_NE(error("copy c to '" + path + "' with format tbl;").find("holds the delimiter"), std::string::npos);
    }

    TEST_F(CopyTest, LoadsIntoBranch) {
        exec("create branch b from main;");
        write("1,a,b,10\n");
        exec("copy c version b from '" + path + "' with format csv;");
        EXPECT_EQ(query("select k from c;"), (rows_t{}));
        EXPECT_EQ(query("select k from c;", "b"), (rows_t{"(1)"}));
        exec("copy c version b to '" + path + "' with format csv;");
        EXPECT_EQ(read(), "1,a,b,10\n");
        EXPECT_NE(error("copy c version nobranch from '" + path + "' with format csv;").find("does not exist"), std::string::npos);
    }

    // COPY FROM checks every record before it inserts one
    TEST_F(CopyTest, ErrorsLeaveTheTableAsItWas) {
        auto expectError = [&](const std::string & content, const std::string & message) {
            write(content);
            const std::string what = error("copy c from '" + path + "' with format csv;");
            EXPECT_NE(what.find(message), std::string::npos) << what;
            EXPECT_EQ(query("select k from c;"), (rows_t{}));
        };
        expectError("1,a,b,1\n2,a,b,2\nthree,a,b,3\n", "line 3: column \"k\"");
        expectError("1,a,b,1\n2,a,b\n", "line 2: missing data for column \"x\"");
        expectError("1,a,b,1,5\n", "line 1: extra data after last expected column");
        expectError("1,a,\"b\n2,a,b,2\n", "unterminated CSV quoted field");
        expectError("1,a,\"b\nc\",1\n,a,b,2\n", "line 3: null value in column \"k\" violates not-null constraint");
        write("1,a,b,1\n,a,b,2\n");
        EXPECT_THROW(exec("copy c from '" + path + "' with format csv;"), semanticalAnalysis::not_null_violation);
        EXPECT_NE(error("copy c from '/nonexistent/tardisdb.csv' with format csv;").find("could not open"), std::string::npos);
    }

    TEST_F(CopyTest, RejectsInvalidOptions) {
        write("1,a,b,1\n");
        const std::string from = "copy c from '" + path + "' ";
        EXPECT_THROW(exec(from + ";"), semanticalAnalysis::semantic_sql_error); // text: one field of tabs
        EXPECT_THROW(exec(from + "with format binary;"), tardisParser::syntactical_error);
        EXPECT_THROW(exec(from + "with ( format csv , freeze );"), tardisParser::syntactical_error);
        EXPECT_THROW(exec(from + "with ( format csv , delimiter ',,' );"), semanticalAnalysis::semantic_sql_error);
        EXPECT_THROW(exec(from + "with ( format csv , delimiter '\"' );"), semanticalAnalysis::semantic_sql_error);
        EXPECT_THROW(exec(from + "with ( format tbl , quote '\"' );"), semanticalAnalysis::semantic_sql_error);
        EXPECT_THROW(exec(from + "with ( format csv , null ',' );"), semanticalAnalysis::semantic_sql_error);
        EXPECT_EQ(query("select k from c;"), (rows_t{}));
    }
}
