//
// Created by Blum Thomas on 2020-06-28.
//

#ifndef PROTODB_PARSERRESULT_HPP
#define PROTODB_PARSERRESULT_HPP

#include <vector>
#include <string>

namespace tardisParser {
    struct ColumnSpec {
        std::string name;
        std::string type;
        size_t length;
        size_t precision;
        bool nullable;
    };
    struct CreateTableStatement {
        std::string tableName;
        std::vector<ColumnSpec> columns;
    };
    struct CreateBranchStatement {
        std::string branchName;
        std::string parentBranchName;
    };
    struct Table {
        std::string name;
        std::string alias;
        std::string version;
    };
    struct Column {
        std::string name;
        std::string table;
    };
    struct SelectStatement {
        std::vector<Column> projections;
        std::vector<Table> relations;
        std::vector<std::pair<Column,Column>> joinConditions;
        std::vector<std::pair<Column,std::string>> selections;
    };
    struct UpdateStatement {
        Table relation;
        std::vector<std::pair<Column,std::string>> updates;
        std::vector<std::pair<Column,std::string>> selections;
    };
    struct InsertStatement {
        Table relation;
        std::vector<Column> columns;
        std::vector<std::string> values;
    };
    struct DeleteStatement {
        Table relation;
        std::vector<std::pair<Column,std::string>> selections;
    };

    using BindingAttribute = std::pair<std::string, std::string>; // bindingName and attribute

    struct SQLParserResult {

        enum OpType : unsigned int {
            Select, Insert, Update, Delete, CreateTable, CreateBranch
        } opType;

        CreateTableStatement *createTableStmt;
        CreateBranchStatement *createBranchStmt;
        InsertStatement *insertStmt;
        SelectStatement *selectStmt;
        UpdateStatement *updateStmt;
        DeleteStatement *deleteStmt;

        SQLParserResult() {}
        ~SQLParserResult() {
            switch (opType) {
                case Select:
                    delete selectStmt;
                    break;
                case Insert:
                    delete insertStmt;
                    break;
                case Update:
                    delete updateStmt;
                    break;
                case Delete:
                    delete deleteStmt;
                    break;
                case CreateTable:
                    delete createTableStmt;
                    break;
                case CreateBranch:
                    delete createBranchStmt;
                    break;
            }
        }
    };
}

#endif //PROTODB_PARSERRESULT_HPP
