#include <sstream>
#include <vector>
#include <iostream>
#include <fstream>
#include <set>
#include "gtest/gtest.h"

#if !USE_HYRISE
#include "sqlParser/SQLParser.hpp"

namespace {

    TEST(SqlParserTest, SelectStatment) {
        std::string statement = "SELECT name FROM professoren p;";

        tardisParser::ParsingContext::OpType opType = tardisParser::ParsingContext::OpType::Select;
        std::string relationName = "professoren";
        std::string bindingName = "p";
        std::string version = "master";
        std::string projection = "name";

        tardisParser::ParsingContext result;
        tardisParser::SQLParser::parseStatement(result, statement);
        tardisParser::SelectStatement* stmt = result.selectStmt;
        ASSERT_EQ(result.opType, opType);
        ASSERT_EQ(stmt->relations[0].name, relationName);
        ASSERT_EQ(stmt->relations[0].alias, bindingName);
        ASSERT_EQ(stmt->relations[0].version, version);
        ASSERT_EQ(stmt->projections[0].name, projection);
    }

    TEST(SqlParserTest, SelectStatmentProjections) {
        std::string statement = "SELECT persnr, name, rang, raum FROM professoren p;";

        tardisParser::ParsingContext::OpType opType = tardisParser::ParsingContext::OpType::Select;
        std::string relationName = "professoren";
        std::string bindingName = "p";
        std::string version = "master";
        std::set<std::string> projections = { "persnr", "name", "rang", "raum" };

        tardisParser::ParsingContext result;
        tardisParser::SQLParser::parseStatement(result, statement);
        tardisParser::SelectStatement* stmt = result.selectStmt;
        ASSERT_EQ(result.opType, opType);
        ASSERT_EQ(stmt->relations[0].name, relationName);
        ASSERT_EQ(stmt->relations[0].alias, bindingName);
        ASSERT_EQ(stmt->relations[0].version, version);

        for (auto &projection : stmt->projections) {
            ASSERT_NE(projections.find(projection.name),projections.end());
        }
    }

    TEST(SqlParserTest, SelectStatmentProjectionStar) {
        std::string statement = "SELECT * FROM professoren p;";

        tardisParser::ParsingContext::OpType opType = tardisParser::ParsingContext::OpType::Select;
        std::string relationName = "professoren";
        std::string bindingName = "p";
        std::string version = "master";

        tardisParser::ParsingContext result;
        tardisParser::SQLParser::parseStatement(result, statement);
        tardisParser::SelectStatement* stmt = result.selectStmt;
        ASSERT_EQ(result.opType, opType);
        ASSERT_EQ(stmt->relations[0].name, relationName);
        ASSERT_EQ(stmt->relations[0].alias, bindingName);
        ASSERT_EQ(stmt->relations[0].version, version);
        ASSERT_TRUE(stmt->projections.empty());
    }

    TEST(SqlParserTest, SelectStatmentMultiRelations) {
        std::string statement = "SELECT * FROM professoren p, studenten s;";

        tardisParser::ParsingContext::OpType opType = tardisParser::ParsingContext::OpType::Select;
        std::set<std::string> relationNames = { "professoren", "studenten"};
        std::set<std::string> relationBindings = { "p", "s"};
        std::string version = "master";

        tardisParser::ParsingContext result;
        tardisParser::SQLParser::parseStatement(result, statement);
        tardisParser::SelectStatement* stmt = result.selectStmt;
        ASSERT_EQ(result.opType, opType);
        for (auto &relation : stmt->relations) {
            ASSERT_NE(relationNames.find(relation.name),relationNames.end());
            ASSERT_NE(relationBindings.find(relation.alias),relationNames.end());
        }
        ASSERT_EQ(stmt->relations[0].version, version);
        ASSERT_TRUE(stmt->projections.empty());
    }

    TEST(SqlParserTest, SelectStatmentWhereNumberCompare) {
        std::string statement = "SELECT * FROM professoren p WHERE rang = 4;";

        tardisParser::ParsingContext::OpType opType = tardisParser::ParsingContext::OpType::Select;
        std::string relationName = "professoren";
        std::string bindingName = "p";
        std::string version = "master";
        std::string whereColumn = "rang";
        std::string whereValue = "4";

        tardisParser::ParsingContext result;
        tardisParser::SQLParser::parseStatement(result, statement);
        tardisParser::SelectStatement* stmt = result.selectStmt;
        ASSERT_EQ(result.opType, opType);
        ASSERT_EQ(stmt->relations[0].name, relationName);
        ASSERT_EQ(stmt->relations[0].alias, bindingName);
        ASSERT_EQ(stmt->relations[0].version, version);
        ASSERT_TRUE(stmt->projections.empty());
        ASSERT_EQ(stmt->selections[0].first.name, whereColumn);
        ASSERT_EQ(stmt->selections[0].second, whereValue);
    }

}  // namespace

#endif
