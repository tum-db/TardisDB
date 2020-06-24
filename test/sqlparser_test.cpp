#include <sstream>
#include <vector>
#include <iostream>
#include <fstream>
#include <set>
#include "gtest/gtest.h"

#include "sqlParser/SQLParser.hpp"

namespace {

    TEST(SqlParserTest, SelectStatment) {
        std::string statement = "SELECT name FROM professoren p;";

        SQLParserResult::OpType opType = SQLParserResult::OpType::Select;
        std::string relationName = "professoren";
        std::string bindingName = "p";
        std::string version = "master";
        std::string projection = "name";

        SQLParserResult result = parse_sql_statement(statement);
        ASSERT_EQ(result.opType, opType);
        ASSERT_EQ(result.relations[0].first, relationName);
        ASSERT_EQ(result.relations[0].second, bindingName);
        ASSERT_EQ(result.projections[0], projection);
        ASSERT_EQ(result.versions[0], version);
    }

    TEST(SqlParserTest, SelectStatmentProjections) {
        std::string statement = "SELECT persnr, name, rang, raum FROM professoren p;";

        SQLParserResult::OpType opType = SQLParserResult::OpType::Select;
        std::string relationName = "professoren";
        std::string bindingName = "p";
        std::string version = "master";
        std::set<std::string> projections = { "persnr", "name", "rang", "raum" };

        SQLParserResult result = parse_sql_statement(statement);

        ASSERT_EQ(result.opType, opType);
        ASSERT_EQ(result.relations[0].first, relationName);
        ASSERT_EQ(result.relations[0].second, bindingName);
        ASSERT_EQ(result.versions[0], version);

        for (auto &projection : result.projections) {
            ASSERT_NE(projections.find(projection),projections.end());
        }
    }

    TEST(SqlParserTest, SelectStatmentProjectionStar) {
        std::string statement = "SELECT * FROM professoren p;";

        SQLParserResult::OpType opType = SQLParserResult::OpType::Select;
        std::string relationName = "professoren";
        std::string bindingName = "p";
        std::string version = "master";

        SQLParserResult result = parse_sql_statement(statement);

        ASSERT_EQ(result.opType, opType);
        ASSERT_EQ(result.relations[0].first, relationName);
        ASSERT_EQ(result.relations[0].second, bindingName);
        ASSERT_EQ(result.versions[0], version);
        ASSERT_TRUE(result.projections.empty());
    }

    TEST(SqlParserTest, SelectStatmentMultiRelations) {
        std::string statement = "SELECT * FROM professoren p, studenten s;";

        SQLParserResult::OpType opType = SQLParserResult::OpType::Select;
        std::set<std::pair<std::string,std::string>> relationBindings;
        relationBindings.insert(std::make_pair("professoren","p"));
        relationBindings.insert(std::make_pair("studenten","s"));
        std::string version = "master";

        SQLParserResult result = parse_sql_statement(statement);

        ASSERT_EQ(result.opType, opType);
        for (auto &projection : result.relations) {
            ASSERT_NE(relationBindings.find(projection),relationBindings.end());
        }
        ASSERT_EQ(result.versions[0], version);
        ASSERT_TRUE(result.projections.empty());
    }

    TEST(SqlParserTest, SelectStatmentWhereNumberCompare) {
        std::string statement = "SELECT * FROM professoren p WHERE rang = 4;";

        SQLParserResult::OpType opType = SQLParserResult::OpType::Select;
        std::string relationName = "professoren";
        std::string bindingName = "p";
        std::string version = "master";
        std::string whereColumn = "rang";
        std::string whereValue = "4";

        SQLParserResult result = parse_sql_statement(statement);

        ASSERT_EQ(result.opType, opType);
        ASSERT_EQ(result.relations[0].first, relationName);
        ASSERT_EQ(result.relations[0].second, bindingName);
        ASSERT_EQ(result.versions[0], version);
        ASSERT_TRUE(result.projections.empty());
        ASSERT_EQ(result.selections[0].first.second, whereColumn);
        ASSERT_EQ(result.selections[0].second, whereValue);
    }

}  // namespace
