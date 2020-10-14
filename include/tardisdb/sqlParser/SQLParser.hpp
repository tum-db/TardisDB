#ifndef PROTODB_SQLPARSER_HPP
#define PROTODB_SQLPARSER_HPP

#include <string>

#include "sqlParser/Tokenizer.hpp"
#include "sqlParser/ParserResult.hpp"

namespace tardisParser {
    struct incorrect_sql_error : std::runtime_error {
        //semantic or syntactic errors
        using std::runtime_error::runtime_error;
    };

// Define all parser states
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

        Done
    } state_t;

    class SQLParser {
    public:

        static void parse_sql_statement(ParsingContext &context, std::string &sql);

    private:

        static bool equals_keyword(const Token &tok, std::string keyword);

        static bool equals_controlSymbol(const Token &tok, std::string controlSymbol);

        static BindingAttribute parse_binding_attribute(std::string value);

        static state_t parse_next_token(Tokenizer &token_src, const state_t state, ParsingContext &query);
    };
}

#endif //PROTODB_SQLPARSER_HPP