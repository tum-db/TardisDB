// Table functions: the registry, and a function of two columns in FROM, under aliases, in joins, and its errors

#include "codegen/LLVMCompat.hpp"
#include <algorithm>
#include <mutex>
#include "runtime/SqlTuple.hpp"
#include "queryCompiler/queryCompiler.hpp"
#include "queryCompiler/TableFunctions.hpp"
#include "semanticAnalyser/SemanticAnalyser.hpp"
#include <gtest/gtest.h>

namespace {

    using rows_t = std::vector<std::string>;

    /// test_pairs(n): the rows (i, 'label i') for i from 1 to n
    class Pairs : public TableFunctions::TableFunction {
    public:
        std::string name() const override { return "test_pairs"; }

        std::vector<TableFunctions::Column> columns(std::vector<TableFunctions::Argument> & arguments) const override {
            if (arguments.size() != 1 || (!arguments[0].null && arguments[0].type.typeID != Sql::SqlType::TypeID::IntegerID)) {
                throw semanticalAnalysis::semantic_sql_error("test_pairs takes an integer");
            }
            return {{"i", Sql::getIntegerTy(false)}, {"label", Sql::getTextTy(false)}};
        }

        void rows(const std::vector<TableFunctions::Argument> & arguments, const std::vector<TableFunctions::Column> & columns,
                  const std::function<void(std::vector<Native::Sql::value_op_t>)> & row) const override {
            if (arguments[0].null) return;
            for (int i = 1; i <= std::stoi(arguments[0].value); ++i) {
                std::vector<Native::Sql::value_op_t> values;
                values.push_back(Native::Sql::castColumnValue(std::to_string(i), columns[0].type));
                values.push_back(Native::Sql::castColumnValue("label " + std::to_string(i), columns[1].type));
                row(std::move(values));
            }
        }
    };

    class TableFunctionTest : public ::testing::Test {
    protected:
        static void SetUpTestCase() { TableFunctions::add(std::make_unique<Pairs>()); }

        void SetUp() override {
            db = std::make_unique<Database>();
            exec("CREATE TABLE tf ( id integer not null, name varchar(10) not null );");
            exec("INSERT INTO tf VALUES (1, 'one'), (2, 'two'), (5, 'five');");
        }

        QueryCompiler::StatementResult exec(const std::string & sql) {
            return QueryCompiler::execute(sql, *db, "master", [](Native::Sql::SqlTuple *) { });
        }

        rows_t sorted(const std::string & sql, std::vector<std::string> * names = nullptr) {
            rows_t rows;
            std::mutex mutex;
            QueryCompiler::execute(sql, *db, "master", [&](Native::Sql::SqlTuple * tuple) {
                std::lock_guard<std::mutex> lock(mutex);
                rows.push_back(Native::Sql::toString(*tuple));
            }, [&](const QueryCompiler::StatementResult & result) { if (names) *names = result.columnNames; });
            std::sort(rows.begin(), rows.end());
            return rows;
        }

        std::unique_ptr<Database> db;
    };

    TEST_F(TableFunctionTest, Registry) {
        EXPECT_NE(TableFunctions::find("GENERATE_SERIES"), nullptr);
        EXPECT_NE(TableFunctions::find("test_pairs"), nullptr);
        EXPECT_EQ(TableFunctions::find("no_such_function"), nullptr);
        // a function of an existing name stays
        const TableFunctions::TableFunction * pairs = TableFunctions::find("test_pairs");
        EXPECT_FALSE(TableFunctions::add(std::make_unique<Pairs>()));
        EXPECT_EQ(TableFunctions::find("test_pairs"), pairs);
    }

    TEST_F(TableFunctionTest, Columns) {
        std::vector<std::string> names;
        EXPECT_EQ(sorted("SELECT * FROM test_pairs(2);", &names), (rows_t{"(1, label 1)", "(2, label 2)"}));
        EXPECT_EQ(names, (std::vector<std::string>{"i", "label"}));
        // the alias names the table; a list of the alias the columns, also only the first
        EXPECT_EQ(sorted("SELECT p.label FROM test_pairs(2) p WHERE p.i = 2;"), (rows_t{"(label 2)"}));
        EXPECT_EQ(sorted("SELECT * FROM test_pairs(1) AS p(n, l);", &names), (rows_t{"(1, label 1)"}));
        EXPECT_EQ(names, (std::vector<std::string>{"n", "l"}));
        EXPECT_EQ(sorted("SELECT p.n, p.label FROM test_pairs(1) p(n);"), (rows_t{"(1, label 1)"}));
        EXPECT_EQ(sorted("SELECT * FROM test_pairs(NULL);"), rows_t{});
        // joins, also with generate_series
        EXPECT_EQ(sorted("SELECT tf.name, p.label FROM tf, test_pairs(3) p WHERE tf.id = p.i;"), (rows_t{"(one, label 1)", "(two, label 2)"}));
        EXPECT_EQ(sorted("SELECT p.label, g FROM test_pairs(2) p, generate_series(2, 3) g WHERE p.i = g;"), (rows_t{"(label 2, 2)"}));
    }

    TEST_F(TableFunctionTest, Errors) {
        EXPECT_THROW(exec("SELECT * FROM test_pairs('a');"), std::exception);
        EXPECT_THROW(exec("SELECT * FROM test_pairs(1, 2);"), std::exception);
        EXPECT_THROW(exec("SELECT * FROM test_pairs(1) p(a, b, c);"), std::exception);
        // several columns are no column of a select list
        EXPECT_THROW(exec("SELECT test_pairs(2);"), std::exception);
        EXPECT_THROW(exec("SELECT * FROM tf WHERE tf.id = test_pairs(1);"), std::exception);
    }

} // namespace
