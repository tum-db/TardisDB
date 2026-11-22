//
// Created by Blum Thomas on 2020-06-28.
//

#ifndef PROTODB_PARSINGCONTEXT_HPP
#define PROTODB_PARSINGCONTEXT_HPP

#include <deque>

#include "sqlParser/Expression.hpp"
#include <vector>
#include <string>

namespace tardisParser {

    typedef enum State : unsigned int {
        Init,

        Select,
        SelectProjectionStar,
        SelectProjectionAttrName,
        SelectProjectionAttrSeparator,
        SelectFrom,
        SelectFromRelationName,
        SelectFromVersion,
        SelectFromTag,
        SelectFromBindingName,
        SelectFromSeparator,
        SelectWhere,
        SelectWhereExprLhs,
        SelectWhereExprOp,
        SelectWhereExprRhs,
        SelectWhereAnd,
        SelectProjectionAggBegin,
        SelectProjectionAggArg,
        SelectFromVersionSetBegin,
        SelectFromVersionSetName,
        SelectFromVersionSetSeparator,
        SelectFromVersionAll,
        SelectFromVersionAllScope,
        SelectFromVersionAllOf,
        SelectGroup,
        SelectGroupBy,
        SelectGroupByName,
        SelectGroupBySeparator,
        SelectHaving,

        Insert,
        InsertInto,
        InsertRelationName,
        InsertVersion,
        InsertTag,
        InsertColumnsBegin,
        InsertColumnsEnd,
        InsertColumnName,
        InsertColumnSeperator,
        InsertValues,
        InsertValuesBegin,
        InsertValuesEnd,
        InsertValue,
        InsertValueSeperator,

        Update,
        UpdateRelationName,
        UpdateVersion,
        UpdateTag,
        UpdateSet,
        UpdateSetExprLhs,
        UpdateSetExprOp,
        UpdateSetExprRhs,
        UpdateSetSeperator,
        UpdateWhere,
        UpdateWhereExprLhs,
        UpdateWhereExprOp,
        UpdateWhereExprRhs,
        UpdateWhereAnd,

        Delete,
        DeleteFrom,
        DeleteRelationName,
        DeleteVersion,
        DeleteTag,
        DeleteWhere,
        DeleteWhereExprLhs,
        DeleteWhereExprOp,
        DeleteWhereExprRhs,
        DeleteWhereAnd,

        Create,
        CreateTable,
        CreateTableRelationName,
        CreateTableColumnsBegin,
        CreateTableColumnsEnd,
        CreateTableColumnName,
        CreateTableColumnType,
        CreateTableTypeDetailBegin,
        CreateTableTypeDetailEnd,
        CreateTableTypeDetailSeperator,
        CreateTableTypeDetailLength,
        CreateTableTypeDetailPrecision,
        CreateTableTypeNot,
        CreateTableTypeNotNull,
        CreateTableColumnSeperator,
        CreateBranch,
        CreateBranchTag,
        CreateBranchFrom,
        CreateBranchParent,

        Branch,

        Copy,
        CopyTable,
        CopyVersion,
        CopyTag,
        CopyDirection,
        CopyPath,
        CopyWith,
        CopyFormat,
        CopyType,

        // SET c = c - 5: an arithmetic operator between two terms
        UpdateSetExprArith,

        // FROM DIFF ( t VERSION a , t VERSION b )
        SelectFromDiffBegin,
        SelectFromDiffTable,
        SelectFromDiffVersion,
        SelectFromDiffName,
        SelectFromDiffSeparator,

        CreateIndex,
        CreateIndexName,
        CreateIndexOn,
        CreateIndexTable,
        CreateIndexColumnsBegin,
        CreateIndexColumnName,
        CreateIndexColumnSeparator,
        CreateIndexColumnsEnd,
        CreateIndexVersion,
        CreateIndexTag,

        Drop,
        DropBranch,
        DropBranchName,
        DropBranchCascade,

        Merge,
        MergeBranch,
        MergeBranchName,
        MergeBranchInto,
        MergeBranchTarget,

        Alter,
        AlterTable,
        AlterTableName,
        AlterTableVersion,
        AlterTableTag,
        AlterTableAdd,
        AlterTableAddColumn,
        AlterTableColumnName,
        AlterTableColumnType,
        AlterTableTypeDetailBegin,
        AlterTableTypeDetailLength,
        AlterTableTypeDetailSeparator,
        AlterTableTypeDetailPrecision,
        AlterTableTypeDetailEnd,
        AlterTableTypeNot,
        AlterTableTypeNotNull,
        AlterTableDefault,
        AlterTableDefaultValue,
        AlterTableDrop,
        AlterTableDropColumn,
        AlterTableDropName,

        Done
    } state_t;

    struct ColumnSpec {
        std::string name;
        std::string type;
        size_t length;
        size_t precision;
        bool nullable;
    };
    struct Table {
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
        std::vector<Table> relations;
        std::vector<std::pair<Column,Column>> joinConditions;
        std::vector<std::pair<Column,std::string>> selections;
        std::vector<Column> groupBy;
        std::vector<Predicate> predicates;
        sqlExpressions::node_p_t having; // the condition of HAVING, if any
    };
    struct UpdateStatement {
        Table relation;
        std::vector<std::pair<Column,std::string>> updates;
        std::vector<std::pair<Column,std::string>> selections;
        std::vector<std::vector<UpdateTerm>> expressions; // the right-hand side of each update
        std::vector<sqlExpressions::node_p_t> trees; // or any expression (then without terms)
        std::vector<Predicate> predicates;
    };
    struct InsertStatement {
        Table relation;
        std::vector<Column> columns;
        std::vector<std::string> values;
        std::vector<std::vector<std::string>> rows; // the rows of VALUES after the first
    };
    struct DeleteStatement {
        Table relation;
        std::vector<std::pair<Column,std::string>> selections;
        std::vector<Predicate> predicates;
    };
    struct CopyStatement {
        Table relation;
        std::string filePath;
        std::string format;
        bool directionFrom;
        /// the fields read by COPY FROM (as semanticalAnalysis::CopyStatement, which this struct is cast to)
        std::deque<std::vector<std::string>> rows;
    };

    struct CreateIndexStatement {
        std::string indexName;
        Table relation;
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
        Table relation;
        bool add = true;
        ColumnSpec column;
        std::string defaultValue;
        bool hasDefault = false;
    };

    using BindingAttribute = std::pair<std::string, std::string>; // bindingName and attribute

    struct ParsingContext {

        State state;

        enum OpType : unsigned int {
            Unkown, Select, Insert, Update, Delete, CreateTable, CreateBranch, Branch, Copy,
            CreateIndex, DropBranch, MergeBranch, AlterTable
        } opType;

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

        /// the branch of the relations a statement names without VERSION (a session's current branch)
        std::string defaultBranch = "master";

        ParsingContext() {
            opType = Unkown;
        }
        ~ParsingContext() {
            switch (opType) {
                case Unkown:
                    break;
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
            }
        }

        bool stateIsFinal() {
            std::set<State> finalStates = { State::SelectFromRelationName,
                                            State::SelectFromTag,
                                            State::SelectFromBindingName,
                                            State::SelectWhereExprRhs,
                                            State::SelectFromVersionAll,
                                            State::SelectGroupByName,
                                            State::InsertValuesEnd,
                                            State::UpdateSetExprRhs,
                                            State::UpdateWhereExprRhs,
                                            State::DeleteRelationName,
                                            State::DeleteTag,
                                            State::DeleteWhereExprRhs,
                                            State::CreateTableRelationName,
                                            State::CreateTableColumnsEnd,
                                            State::CreateBranchParent,
                                            State::Branch,
                                            State::CopyType,
                                            State::CreateIndexColumnsEnd,
                                            State::CreateIndexTag,
                                            State::DropBranchName,
                                            State::DropBranchCascade,
                                            State::MergeBranchTarget,
                                            State::AlterTableColumnType,
                                            State::AlterTableTypeDetailEnd,
                                            State::AlterTableTypeNotNull,
                                            State::AlterTableDefaultValue,
                                            State::AlterTableDropName };

            return finalStates.count(state);
        }
    };
}

#endif //PROTODB_PARSINGCONTEXT_HPP
