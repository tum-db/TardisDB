//
// Created by Blum Thomas on 2020-06-28.
//

#ifndef PROTODB_PARSERRESULT_HPP
#define PROTODB_PARSERRESULT_HPP

#include <vector>
#include <string>

namespace semanticalAnalysis {

    struct ColumnSpec {
        std::string name;
        std::string type;
        size_t length;
        size_t precision;
        bool nullable;
    };
    struct Relation {
        std::string name;
        std::string alias;
        std::string version;
        // branch-aware queries: "" (only version), "set" (versionSet), "all" (every branch),
        // "descendants" or "leaves" (of version)
        std::string versionScope;
        std::vector<std::string> versionSet;
    };
    struct Column {
        std::string name;
        std::string table;
        std::string aggregate; // "", "count", "sum", "min", "max" or "avg"; count(*) has name "*"
    };

    struct CreateTableStatement {
        std::string tableName;
        std::vector<ColumnSpec> columns;
    };
    struct CreateBranchStatement {
        std::string branchName;
        std::string parentBranchName;
    };
    struct SelectStatement {
        std::vector<Column> projections;
        std::vector<Relation> relations;
        std::vector<std::pair<Column,Column>> joinConditions;
        std::vector<std::pair<Column,std::string>> selections;
        std::vector<Column> groupBy;
    };
    struct UpdateStatement {
        Relation relation;
        std::vector<std::pair<Column,std::string>> updates;
        std::vector<std::pair<Column,std::string>> selections;
    };
    struct InsertStatement {
        Relation relation;
        std::vector<Column> columns;
        std::vector<std::string> values;
    };
    struct DeleteStatement {
        Relation relation;
        std::vector<std::pair<Column,std::string>> selections;
    };
    struct CopyStatement {
        Relation relation;
        std::string filePath;
        std::string format;
        bool directionFrom;
    };

    using BindingAttribute = std::pair<std::string, std::string>; // bindingName and attribute

    struct SQLParserResult {

        enum OpType : unsigned int {
            Unknown, Select, Insert, Update, Delete, CreateTable, CreateBranch, Branch, Copy
        } opType = Unknown;

        CreateTableStatement *createTableStmt;
        CreateBranchStatement *createBranchStmt;
        InsertStatement *insertStmt;
        SelectStatement *selectStmt;
        UpdateStatement *updateStmt;
        DeleteStatement *deleteStmt;
        CopyStatement *copyStmt;

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
                case Branch:
                    break;
                case Copy:
                    delete copyStmt;
                    break;
                case Unknown:
                    break;
            }
        }
    };
}

#endif //PROTODB_PARSERRESULT_HPP
