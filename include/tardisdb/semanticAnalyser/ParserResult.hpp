//
// Created by Blum Thomas on 2020-06-28.
//

#ifndef PROTODB_PARSERRESULT_HPP
#define PROTODB_PARSERRESULT_HPP


#include "sqlParser/Expression.hpp"
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
        // "descendants" or "leaves" (of version), "diff" (the two versions in versionSet)
        std::string versionScope;
        std::vector<std::string> versionSet;
    };
    struct Column {
        std::string name;
        std::string table;
        std::string aggregate; // "", "count", "sum", "min", "max" or "avg"; count(*) has name "*"
        std::string alias; // AS alias of the select list
        sqlExpressions::node_p_t expression; // a computed column of the select list (name: its default name)
    };

    /// A condition of WHERE other than an equality of a column with a constant (those are
    /// selections): columns op values, where op is <, <=, >, >=, <> or "in". More than one column is
    /// a row value, compared lexicographically, e.g. (a, b) >= (1, 2); "in" has one column and a list.
    struct Predicate {
        std::vector<Column> columns;
        std::string op;
        std::vector<std::string> values;
        sqlExpressions::node_p_t expression; // op "expr": any condition
    };

    /// A term of the right-hand side of SET: a column or a constant, preceded by an operator
    /// ("+", "-", "*" or "/") unless it is the first term
    struct UpdateTerm {
        std::string op;
        Column column;     // empty name for a constant
        std::string value;
    };

    struct CreateTableStatement {
        std::string tableName;
        std::vector<ColumnSpec> columns;
        std::vector<std::string> primaryKey; // the columns of PRIMARY KEY ( ... ), if any
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
        std::vector<Predicate> predicates;
        sqlExpressions::node_p_t having; // the condition of HAVING, if any
    };
    struct UpdateStatement {
        Relation relation;
        std::vector<std::pair<Column,std::string>> updates;
        std::vector<std::pair<Column,std::string>> selections;
        std::vector<std::vector<UpdateTerm>> expressions; // the right-hand side of each update
        std::vector<sqlExpressions::node_p_t> trees; // or any expression (then without terms)
        std::vector<Predicate> predicates;
    };
    struct InsertStatement {
        Relation relation;
        std::vector<Column> columns;
        std::vector<std::string> values;
        std::vector<std::vector<std::string>> rows; // the rows of VALUES after the first
    };
    struct DeleteStatement {
        Relation relation;
        std::vector<std::pair<Column,std::string>> selections;
        std::vector<Predicate> predicates;
    };
    struct CopyStatement {
        Relation relation;
        std::string filePath;
        std::string format;      // csv or tbl
        bool directionFrom;
        bool header = false;     // the first line names the columns
        std::string delimiter;   // the options; empty for the default of the format
        std::string quote;
        std::string escape;
        std::string null;        // the text of NULL (an empty field by default)
    };

    using BindingAttribute = std::pair<std::string, std::string>; // bindingName and attribute

    struct CreateIndexStatement {
        std::string indexName;
        Relation relation;
        std::vector<Column> columns;
    };
    struct DropBranchStatement {
        std::string branchName;
        bool cascade = false;
    };
    struct MergeBranchStatement {
        std::string branchName;
        std::string targetBranchName;
        std::string onConflict; // "", or ON CONFLICT KEEP "target" | "source"
    };

    /// ALTER TABLE t [ VERSION b ] ADD [ COLUMN ] c type [ NOT NULL ] DEFAULT v, or DROP [ COLUMN ] c
    struct AlterTableStatement {
        Relation relation;
        bool add = true;
        ColumnSpec column;
        std::string defaultValue;
        bool hasDefault = false;
    };

    struct SQLParserResult {

        enum OpType : unsigned int {
            Unknown, Select, Insert, Update, Delete, CreateTable, CreateBranch, Branch, Copy,
            CreateIndex, DropBranch, MergeBranch, AlterTable
        } opType = Unknown;

        CreateTableStatement *createTableStmt;
        CreateBranchStatement *createBranchStmt;
        InsertStatement *insertStmt;
        SelectStatement *selectStmt;
        UpdateStatement *updateStmt;
        DeleteStatement *deleteStmt;
        CopyStatement *copyStmt;
        CreateIndexStatement *createIndexStmt;
        DropBranchStatement *dropBranchStmt;
        MergeBranchStatement *mergeBranchStmt;
        AlterTableStatement *alterTableStmt;

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
                case CreateIndex:
                    delete createIndexStmt;
                    break;
                case DropBranch:
                    delete dropBranchStmt;
                    break;
                case MergeBranch:
                    delete mergeBranchStmt;
                    break;
                case AlterTable:
                    delete alterTableStmt;
                    break;
                case Unknown:
                    break;
            }
        }
    };
}

#endif //PROTODB_PARSERRESULT_HPP
