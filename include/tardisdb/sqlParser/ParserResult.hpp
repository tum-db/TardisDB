//
// Created by Blum Thomas on 2020-06-28.
//

#ifndef PROTODB_PARSINGCONTEXT_HPP
#define PROTODB_PARSINGCONTEXT_HPP

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
    };
    struct Column {
        std::string name;
        std::string table;
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
    struct CopyStatement {
        Table relation;
        std::string filePath;
        std::string format;
        bool directionFrom;
    };

    using BindingAttribute = std::pair<std::string, std::string>; // bindingName and attribute

    struct ParsingContext {

        State state;

        enum OpType : unsigned int {
            Unkown, Select, Insert, Update, Delete, CreateTable, CreateBranch, Branch, Copy
        } opType;

        CreateTableStatement *createTableStmt;
        CreateBranchStatement *createBranchStmt;
        InsertStatement *insertStmt;
        SelectStatement *selectStmt;
        UpdateStatement *updateStmt;
        DeleteStatement *deleteStmt;
        CopyStatement *copyStmt;

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
            }
        }

        bool stateIsFinal() {
            std::set<State> finalStates = { State::SelectFromRelationName,
                                            State::SelectFromTag,
                                            State::SelectFromBindingName,
                                            State::SelectWhereExprRhs,
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
                                            State::CopyType };

            return finalStates.count(state);
        }
    };
}

#endif //PROTODB_PARSINGCONTEXT_HPP
