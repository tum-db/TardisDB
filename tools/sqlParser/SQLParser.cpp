#include "sqlParser/SQLParser.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <memory>
#include <optional>
#include <cassert>
#include <unordered_map>

using BindingAttribute = SQLParserResult::BindingAttribute;
using Constant = SQLParserResult::Constant;
using AttributeName = SQLParserResult::AttributeName;
using Relation = SQLParserResult::Relation;

namespace keywords {
    const std::string Select = "select";
    const std::string From = "from";
    const std::string Where = "where";
    const std::string And = "and";

    const std::string Insert = "insert";
    const std::string Into = "into";
    const std::string Values = "values";

    const std::string Update = "update";
    const std::string Set = "set";

    const std::string Delete = "delete";

    const std::string Create = "create";
    const std::string Table = "table";
    const std::string Not = "not";
    const std::string Null = "null";

    const std::string Checkout = "checkout";
    const std::string Branch = "branch";

    std::set<std::string> keywordset = {Select, From, Where, And, Insert, Into, Values, Update, Set, Delete, Create, Table, Not, Null};
}

enum TokenType : unsigned int {
    identifier, keyword, separator, op, literal, comment, delimiter
};

struct Token {
    TokenType type;
    std::string value;
    Token(TokenType type, std::string value) : type(type), value(value)
    { }
};

typedef enum State : unsigned int {
    Init, Select, SelectProjectionStar, SelectProjectionAttrName, SelectProjectionAttrSeparator,
    SelectFrom, SelectFromRelationName, SelectFromBindingName, SelectFromSeparator,
    SelectWhere, SelectWhereExprLhs, SelectWhereExprOp, SelectWhereExprRhs, SelectWhereAnd,

    Insert, InsertInto, InsertRelationName, InsertColumnsBegin, InsertColumnsEnd, InsertColumnName, InsertColumnSeperator,
    InsertValues, InsertValuesBegin, InsertValuesEnd, InsertValue, InsertValueSeperator,

    Update, UpdateRelationName, UpdateBindingName, UpdateSet, UpdateSetExprLhs, UpdateSetExprOp, UpdateSetExprRhs, UpdateSetSeperator,
    UpdateWhere, UpdateWhereExprLhs, UpdateWhereExprOp, UpdateWhereExprRhs, UpdateWhereAnd,

    Delete, DeleteFrom, DeleteRelationName, DeleteBindingName,
    DeleteWhere, DeleteWhereExprLhs, DeleteWhereExprOp, DeleteWhereExprRhs, DeleteWhereAnd,

    Create, CreateTable, CreateTableRelationName, CreateTableColumnsBegin, CreateTableColumnsEnd, CreateTableColumnName, CreateTableColumnType,
    CreateTableTypeDetailBegin, CreateTableTypeDetailEnd, CreateTableTypeDetailSeperator, CreateTableTypeDetailLength, CreateTableTypeDetailPrecision,
    CreateTableTypeNot, CreateTableTypeNotNull, CreateTableColumnSeperator,

    Checkout, CheckoutBranch, CheckoutBranchTag, CheckoutBranchFrom, CheckoutBranchParent,

    Semicolon,
    Done
} state_t;

static bool is_identifier(const Token & tok) {
    return tok.type == TokenType::identifier;
}

static bool is_keyword(const Token & tok, std::string keyword) {
    std::transform(keyword.begin(), keyword.end(), keyword.begin(), ::tolower);
    return (tok.type == TokenType::keyword && tok.value == keyword);
}

static bool is_identifier(const std::string& str) {
    if (str==keywords::Select ||
        str==keywords::From ||
        str==keywords::Where ||
        str==keywords::And ||
        str==keywords::Insert ||
        str==keywords::Into ||
        str==keywords::Values ||
        str==keywords::Update ||
        str==keywords::Set ||
        str==keywords::Delete ||
        std::isdigit(str[0]) ||
        str[0] == '.' ||
        str[0] == '_'
    ) {
        return false;
    }
    return str.find_first_not_of("abcdefghijklmnopqrstuvwxyz_.1234567890") == std::string::npos;
}

static bool is_escaped(const std::string & str) {
    if (str.length() > 2 && str.front() == '"') {
        return (str.back() == '"');
    } else if (str.length() > 2 && str.front() == '\'') {
        return (str.back() == '\'');
    }
    return false;
}

static std::string unescape(const std::string & str) {
    return str.substr(1, str.size() - 2);
}

BindingAttribute parse_binding_attribute(std::string value) {
//    std::cout << "binding attr: " << value << std::endl;
    auto dotPos = value.find_first_of(".");
    if (dotPos == std::string::npos) {
//        throw incorrect_sql_error("invalid binding attribute");
        return BindingAttribute({}, value);
    }

    auto binding = value.substr(0, dotPos);
    auto attribute = value.substr(dotPos+1, value.size());
    return BindingAttribute(binding, attribute);
}

struct Tokenizer {
    std::istream & _in;
    std::vector<Token> _tokens;

    Tokenizer(std::istream & in) : _in(in)
    { }

    const Token & prev(size_t i = 1) {
        assert(_tokens.size() >= i);
        return _tokens[_tokens.size()-1-i];
    }

    const Token & next() {
        std::stringstream ss;

        for (size_t i = 0;; ++i) {
            int c = _in.get();
            if (c == EOF) {
                if (i == 0 && _tokens.back().type != TokenType::delimiter) {
                    ss << ";";
                }
                break;
            } else if (std::isspace(c)) {
                if (i > 0) {
                    break;
                } else {
                    continue;
                }
            } else if (c == ',' || c == ';' || c == '=') {
                if (i > 0) {
                    _in.putback(c);
                } else {
                    ss << static_cast<char>(c);
                }
                break;
            }
            ss << static_cast<char>(c);
        }

        std::string token_value = ss.str();
        if (token_value == ",") {
            _tokens.emplace_back(TokenType::separator, token_value);
        } else if (token_value == ";") {
            _tokens.emplace_back(TokenType::delimiter, token_value);
        } else if (token_value == "=") {
            _tokens.emplace_back(TokenType::op, token_value);
        } else if (keywords::keywordset.count(token_value)) {
            _tokens.emplace_back(TokenType::keyword, token_value);
        } else if (is_identifier(token_value)) {
            _tokens.emplace_back(TokenType::identifier, token_value);
        } else if (is_escaped(token_value)) {
            _tokens.emplace_back(TokenType::literal, unescape(token_value));
        } else {
            _tokens.emplace_back(TokenType::literal, token_value);
        }

        return _tokens.back();
    }
};

static state_t parse_next_token(Tokenizer & token_src, const state_t state, SQLParserResult & query) {
    auto & token = token_src.next();
//    std::cout << "current token: " << token.value << std::endl;
    if (token.type == TokenType::delimiter) {
        if (state != State::SelectFromBindingName &&
            state != State::SelectWhereExprRhs &&
            state != State::InsertValuesEnd &&
            state != State::UpdateSetExprRhs &&
            state != State::UpdateWhereExprRhs &&
            state != State::DeleteRelationName &&
            state != State::DeleteWhereExprRhs &&
            state != State::CreateTableRelationName &&
            state != State::CreateTableColumnsEnd &&
            state != State::CheckoutBranchParent &&
            state != State::Semicolon) {
            throw incorrect_sql_error("unexpected end of input");
        }
        return State::Done;
    }

    state_t new_state;
    std::string token_value = token.value;
    std::string lowercase_token_value;
    std::transform(token_value.begin(), token_value.end(), std::back_inserter(lowercase_token_value), tolower);
    switch(state) {
    case State::Init:
        if (lowercase_token_value == keywords::Select) {
            query.opType = "select";
            new_state = State::Select;
        } else if (lowercase_token_value == keywords::Insert) {
            query.opType = "insert";
            new_state = State::Insert;
        } else if (lowercase_token_value == keywords::Update) {
            query.opType = "update";
            new_state = State::Update;
        } else if (lowercase_token_value == keywords::Delete) {
            query.opType = "delete";
            new_state = State::Delete;
        } else if (lowercase_token_value == keywords::Create) {
            query.opType = "create";
            new_state = State::Create;
        } else if (lowercase_token_value == keywords::Checkout) {
            query.opType = "checkout";
            new_state = State::Checkout;
        } else {
            throw incorrect_sql_error("Expected 'Select' or 'Insert', found '" + token_value + "'");
        }
        break;
        case State::Checkout:
            if (lowercase_token_value == keywords::Branch) {
                new_state = State::CheckoutBranch;
            } else {
                throw incorrect_sql_error("Expected 'BRANCH', found '" + token_value + "'");
            }
            break;
        case State::CheckoutBranch:
            if (is_identifier(token)) {
                query.branchId = token_value;
                new_state = State::CheckoutBranchTag;
            } else {
                throw incorrect_sql_error("Expected tag, found '" + token_value + "'");
            }
            break;
        case State::CheckoutBranchTag:
            if (lowercase_token_value == keywords::From) {
                new_state = State::CheckoutBranchFrom;
            } else {
                throw incorrect_sql_error("Expected 'FROM', found '" + token_value + "'");
            }
            break;
        case State::CheckoutBranchFrom:
            if (is_identifier(token)) {
                query.parentBranchId = token_value;
                new_state = State::CheckoutBranchParent;
            } else {
                throw incorrect_sql_error("Expected tag, found '" + token_value + "'");
            }
            break;
        case State::CheckoutBranchParent:
            if (token_value == ";") {
                new_state = State::Semicolon;
            }
            break;

        case State::Create:
            if (lowercase_token_value == keywords::Table) {
                new_state = State::CreateTable;
            } else {
                throw incorrect_sql_error("Expected 'TABLE', found '" + token_value + "'");
            }
            break;
        case State::CreateTable:
            if (is_identifier(token)) {
                query.relation = token_value;
                new_state = State::CreateTableRelationName;
            } else {
                throw incorrect_sql_error("Expected table name, found '" + token_value + "'");
            }
            break;
        case State::CreateTableRelationName:
            if (token_value == "(") {
                new_state = State::CreateTableColumnsBegin;
            }
            break;
        case State::CreateTableColumnsBegin:
        case State::CreateTableColumnSeperator:
            if (is_identifier(token)) {
                query.columnNames.emplace_back(token_value);
                new_state = CreateTableColumnName;
            } else {
                throw incorrect_sql_error("Expected column name, found '" + token_value + "'");
            }
            break;
        case State::CreateTableColumnName:
            if (token.type == literal) {
                query.columnTypes.emplace_back(lowercase_token_value);
                new_state = CreateTableColumnType;
            } else {
                throw incorrect_sql_error("Expected column name, found '" + token_value + "'");
            }
            break;
        case State::CreateTableColumnType:
            if (token_value == ")") {
                query.length.emplace_back(0);
                query.precision.emplace_back(0);
                query.nullable.emplace_back(true);
                new_state = CreateTableColumnsEnd;
            } else if (token_value == ",") {
                query.length.emplace_back(0);
                query.precision.emplace_back(0);
                query.nullable.emplace_back(true);
                new_state = CreateTableColumnSeperator;
            } else if (token_value == "(") {
                new_state = CreateTableTypeDetailBegin;
            } else if (lowercase_token_value == keywords::Not) {
                new_state = CreateTableTypeNot;
            }
            break;
        case State::CreateTableTypeDetailBegin:
            if (token.type == literal) {
                query.length.emplace_back(std::stoi(token_value));
                new_state = CreateTableTypeDetailLength;
            } else {
                throw incorrect_sql_error("Expected column name, found '" + token_value + "'");
            }
            break;
        case State::CreateTableTypeDetailLength:
            if (token_value == ",") {
                new_state = CreateTableTypeDetailSeperator;
            } else if (token_value == ")") {
                query.precision.emplace_back(0);
                new_state = CreateTableTypeDetailEnd;
            } else {
                throw incorrect_sql_error("Expected ',', found '" + token_value + "'");
            }
            break;
        case State::CreateTableTypeDetailSeperator:
            if (token.type == literal) {
                query.precision.emplace_back(std::stoi(token_value));
                new_state = CreateTableTypeDetailPrecision;
            } else {
                throw incorrect_sql_error("Expected column name, found '" + token_value + "'");
            }
            break;
        case State::CreateTableTypeDetailPrecision:
            if (token_value == ")") {
                new_state = CreateTableTypeDetailEnd;
            } else {
                throw incorrect_sql_error("Expected ')', found '" + token_value + "'");
            }
            break;
        case State::CreateTableTypeDetailEnd:
            if (token_value == ")") {
                query.nullable.emplace_back(true);
                new_state = CreateTableColumnsEnd;
            } else if (token_value == ",") {
                query.nullable.emplace_back(true);
                new_state = CreateTableColumnSeperator;
            } else if (lowercase_token_value == keywords::Not) {
                new_state = CreateTableTypeNot;
            }
            break;
        case State::CreateTableTypeNot:
            if (lowercase_token_value == keywords::Null) {
                query.nullable.emplace_back(false);
                new_state = State::CreateTableTypeNotNull;
            } else {
                throw incorrect_sql_error("Expected 'NULL', found '" + token_value + "'");
            }
            break;
        case State::CreateTableTypeNotNull:
            if (token_value == ")") {
                new_state = CreateTableColumnsEnd;
            } else if (token_value == ",") {
                new_state = CreateTableColumnSeperator;
            }
            break;
        case State::CreateTableColumnsEnd:
            if (token_value == ";") {
                new_state = State::Semicolon;
            }
            break;

        case State::Delete:
            if (lowercase_token_value == keywords::From) {
                new_state = State::DeleteFrom;
            } else {
                throw incorrect_sql_error("Expected 'WHERE', found '" + token_value + "'");
            }
            break;
        case State::DeleteFrom:
            if (is_identifier(token)) {
                // token contains a relation name
                using rel_t = decltype(query.relations)::value_type;
                query.relations.push_back(rel_t(token_value, { }));
                new_state = DeleteRelationName;
            } else {
                throw incorrect_sql_error("Expected table name, found '" + token_value + "'");
            }
            break;
        case State::DeleteRelationName:
            if (is_identifier(token)) {
                // token contains the binding name
                using rel_t = decltype(query.relations)::value_type;
                auto current = query.relations.back();
                query.relations.pop_back();
                query.relations.push_back(rel_t(current.first, token_value));
                new_state = State::DeleteBindingName;
            } else {
                throw incorrect_sql_error("Expected binding name after relation name, found '" + token_value + "'");
            }
            break;
        case State::DeleteBindingName:
            if (lowercase_token_value == keywords::Where) {
                new_state = State::DeleteWhere;
            } else {
                throw incorrect_sql_error("Expected 'WHERE', found '" + token_value + "'");
            }
            break;
        case State::DeleteWhere:
        case State::DeleteWhereAnd: /* fallthrough */
            // e.g. s.matrnr will be handled as a single identifier
            if (is_identifier(token)) {
                // attribute binding
                new_state = State::DeleteWhereExprLhs;
            } else {
                throw incorrect_sql_error("Expected identifier after relation reference, found '" + token_value + "'");
            }
            break;
        case State::DeleteWhereExprLhs:
            if (token.type == TokenType::op && token_value == "=") {
                new_state = State::DeleteWhereExprOp;
            } else {
                throw incorrect_sql_error("Expected '=', found '" + token_value + "'");
            }
            break;
        case State::DeleteWhereExprOp:
            if (token.type == TokenType::identifier) {
                std::string lhs = token_src.prev(2).value;
                std::string rhs = token_value;
                query.selectionsWithoutBinding.push_back(std::make_pair(lhs, rhs));
                new_state = State::DeleteWhereExprRhs;
            } else if (token.type == TokenType::literal) {
                std::string lhs = token_src.prev(2).value;
                std::string rhs = token_value;
                query.selectionsWithoutBinding.push_back(std::make_pair(lhs, rhs));
                new_state = State::DeleteWhereExprRhs;
            } else {
                throw incorrect_sql_error("Expected identifier after relation reference, found '" + token_value + "'");
            }
            break;
        case State::DeleteWhereExprRhs:
            if (is_keyword(token, "and")) {
                new_state = State::DeleteWhereAnd;
            } else if (token_value == ";") {
                new_state = State::Semicolon;
            }
            break;
        case State::Update:
            if (is_identifier(token)) {
                // token contains a relation name
                using rel_t = decltype(query.relations)::value_type;
                query.relations.push_back(rel_t(token_value, { }));
                new_state = UpdateRelationName;
            } else {
                throw incorrect_sql_error("Expected table name, found '" + token_value + "'");
            }
            break;
        case State::UpdateRelationName:
            if (is_identifier(token)) {
                // token contains the binding name
                using rel_t = decltype(query.relations)::value_type;
                auto current = query.relations.back();
                query.relations.pop_back();
                query.relations.push_back(rel_t(current.first, token_value));
                new_state = State::UpdateBindingName;
            } else {
                throw incorrect_sql_error("Expected binding name after relation name, found '" + token_value + "'");
            }
            break;
        case State::UpdateBindingName:
            if (lowercase_token_value == keywords::Set) {
                new_state = State::UpdateSet;
            } else {
                throw incorrect_sql_error("Expected 'SET', found '" + token_value + "'");
            }
            break;
        case State::UpdateSet:
        case State::UpdateSetSeperator:
            if (is_identifier(token)) {
                new_state = State::UpdateSetExprLhs;
            } else {
                throw incorrect_sql_error("Expected identifier after relation reference, found '" + token_value + "'");
            }
            break;
        case State::UpdateSetExprLhs:
            if (token.type == TokenType::op && token_value == "=") {
                new_state = State::UpdateSetExprOp;
            } else {
                throw incorrect_sql_error("Expected '=', found '" + token_value + "'");
            }
            break;
        case State::UpdateSetExprOp:
            if (token.type == TokenType::literal) {
                std::string lhs = token_src.prev(2).value;
                query.columnToValue.push_back(std::make_pair(lhs, token_value));
                new_state = State::UpdateSetExprRhs;
            } else {
                throw incorrect_sql_error("Expected identifier after relation reference, found '" + token_value + "'");
            }
            break;
        case State::UpdateSetExprRhs:
            if (token_value == ",") {
                new_state = State::UpdateSetSeperator;
            } else if (lowercase_token_value == keywords::Where) {
                new_state = State::UpdateWhere;
            } else if (token_value == ";") {
                new_state = State::Semicolon;
            }
            break;
        case State::UpdateWhere:
        case State::UpdateWhereAnd: /* fallthrough */
            // e.g. s.matrnr will be handled as a single identifier
            if (is_identifier(token)) {
                // attribute binding
                new_state = State::UpdateWhereExprLhs;
            } else {
                throw incorrect_sql_error("Expected identifier after relation reference, found '" + token_value + "'");
            }
            break;
        case State::UpdateWhereExprLhs:
            if (token.type == TokenType::op && token_value == "=") {
                new_state = State::UpdateWhereExprOp;
            } else {
                throw incorrect_sql_error("Expected '=', found '" + token_value + "'");
            }
            break;
        case State::UpdateWhereExprOp:
            // e.g. s.matrnr will be handled as a single identifier
            if (is_identifier(token)) {
                // attribute binding
                BindingAttribute lhs = parse_binding_attribute(token_src.prev(2).value);
                BindingAttribute rhs = parse_binding_attribute(token_value);
                query.joinConditions.push_back(std::make_pair(lhs, rhs));
                new_state = State::UpdateWhereExprRhs;
            } else if (token.type == TokenType::literal) {
                // constant
                BindingAttribute lhs = parse_binding_attribute(token_src.prev(2).value);
                Constant rhs = unescape(token_value);
                query.selections.push_back(std::make_pair(lhs, token_value));
                new_state = State::UpdateWhereExprRhs;
            } else {
                throw incorrect_sql_error("Expected identifier after relation reference, found '" + token_value + "'");
            }
            break;
        case State::UpdateWhereExprRhs:
            if (is_keyword(token, "and")) {
                new_state = State::UpdateWhereAnd;
            } else if (token_value == ";") {
                new_state = State::Semicolon;
            }
            break;
        case State::Insert:
            if (lowercase_token_value == keywords::Into) {
                new_state = State::InsertInto;
            } else {
                throw incorrect_sql_error("Expected 'INTO', found '" + token_value + "'");
            }
            break;
        case State::InsertInto:
            if (is_identifier(token)) {
                query.relation = token_value;
                new_state = InsertRelationName;
            }
            break;
        case State::InsertRelationName:
            if (token_value == "(") {
                new_state = InsertColumnsBegin;
            } else if (lowercase_token_value == keywords::Values) {
                new_state = InsertValues;
            } else {
                throw incorrect_sql_error("Expected table name, found '" + token_value + "'");
            }
            break;
        case State::InsertColumnsBegin:
        case State::InsertColumnSeperator:
            if (is_identifier(token)) {
                query.columnNames.emplace_back(token_value);
                new_state = InsertColumnName;
            } else {
                throw incorrect_sql_error("Expected column name, found '" + token_value + "'");
            }
            break;
        case State::InsertColumnName:
            if (token_value == ")") {
                new_state = InsertColumnsEnd;
            } else if (token_value == ",") {
                new_state = InsertColumnSeperator;
            }
            break;
        case State::InsertColumnsEnd:
            if (lowercase_token_value == keywords::Values) {
                new_state = InsertValues;
            } else {
                throw incorrect_sql_error("Expected 'VALUES', found '" + token_value + "'");
            }
            break;
        case State::InsertValues:
            if (token_value == "(") {
                new_state = InsertValuesBegin;
            } else {
                throw incorrect_sql_error("Expected ',', found '" + token_value + "'");
            }
            break;
        case State::InsertValuesBegin:
        case State::InsertValueSeperator:
            if (token.type == TokenType::literal) {
                query.values.emplace_back(token_value);
                new_state = InsertValue;
            } else {
                throw incorrect_sql_error("Expected column name, found '" + token_value + "'");
            }
            break;
        case State::InsertValue:
            if (token_value == ",") {
                new_state = InsertValueSeperator;
            } else if (token_value == ")") {
                new_state = InsertValuesEnd;
            }
            break;
    case State::Select: /* fallthrough */
    case State::SelectProjectionAttrSeparator:
        if (token_value == "*") {
            new_state = State::SelectProjectionStar;
        } else if (is_identifier(token)) {
            query.projections.push_back(token_value);
            new_state = SelectProjectionAttrName;
        } else {
            throw incorrect_sql_error("Expected table name, found '" + token_value + "'");
        }
        break;
    case SelectProjectionStar: /* fallthrough */
        if (lowercase_token_value == keywords::From) {
            new_state = State::SelectFrom;
        } else {
            // NOTE: as defined in the given gramma
            throw incorrect_sql_error("Expected 'From' after '*', found '" + token_value + "'");
        }
        break;
    case State::SelectProjectionAttrName:
        if (lowercase_token_value == keywords::From) {
            new_state = State::SelectFrom;
        } else if (token_value == ",") {
            new_state = State::SelectProjectionAttrSeparator;
        } else {
            throw incorrect_sql_error("Expected ',' or 'From' after attribute name, found '" + token_value + "'");
        }
        break;
    case SelectFrom: /* fallthrough */
    case SelectFromSeparator:
        if (is_identifier(token)) {
            // token contains a relation name
            using rel_t = decltype(query.relations)::value_type;
            query.relations.push_back(rel_t(token_value, { }));
            new_state = SelectFromRelationName;
        } else {
            throw incorrect_sql_error("Expected table name, found '" + token_value + "'");
        }
        break;
    case State::SelectFromRelationName:
        if (is_identifier(token)) {
            // token contains the binding name
            using rel_t = decltype(query.relations)::value_type;
            auto current = query.relations.back();
            query.relations.pop_back();
            query.relations.push_back(rel_t(current.first, token_value));
            new_state = State::SelectFromBindingName;
        } else {
            throw incorrect_sql_error("Expected binding name after relation name, found '" + token_value + "'");
        }
        break;
    case State::SelectFromBindingName:
        if (token_value == keywords::Where) {
            new_state = State::SelectWhere;
        } else if (token_value == ",") {
            new_state = State::SelectFromSeparator;
        } else if (token_value == ";") {
            new_state = State::Semicolon;
        } else {
            throw incorrect_sql_error("Expected ',' or 'Where' after relation reference, found '" + token_value + "'");
        }
        break;
    case State::SelectWhere:
    case State::SelectWhereAnd: /* fallthrough */
        // e.g. s.matrnr will be handled as a single identifier
        if (is_identifier(token)) {
            // attribute binding
            new_state = State::SelectWhereExprLhs;
        } else {
            throw incorrect_sql_error("Expected identifier after relation reference, found '" + token_value + "'");
        }
        break;
    case State::SelectWhereExprLhs:
        if (token.type == TokenType::op && token_value == "=") {
            new_state = State::SelectWhereExprOp;
        } else {
            throw incorrect_sql_error("Expected '=', found '" + token_value + "'");
        }
        break;
    case State::SelectWhereExprOp:
        // e.g. s.matrnr will be handled as a single identifier
        if (is_identifier(token)) {
            // attribute binding
            BindingAttribute lhs = parse_binding_attribute(token_src.prev(2).value);
            BindingAttribute rhs = parse_binding_attribute(token_value);
            query.joinConditions.push_back(std::make_pair(lhs, rhs));
            new_state = State::SelectWhereExprRhs;
        } else if (token.type == TokenType::literal) {
            // constant
            BindingAttribute lhs = parse_binding_attribute(token_src.prev(2).value);
            Constant rhs = unescape(token_value);
            query.selections.push_back(std::make_pair(lhs, token_value));
            new_state = State::SelectWhereExprRhs;
        } else {
            throw incorrect_sql_error("Expected identifier after relation reference, found '" + token_value + "'");
        }
        break;
    case State::SelectWhereExprRhs:
        if (is_keyword(token, "and")) {
            new_state = State::SelectWhereAnd;
        } else if (token_value == ";") {
            new_state = State::Semicolon;
        }
        break;
    case State::Semicolon:
        new_state = State::Done;
        break;
    default:
        throw incorrect_sql_error("unexpected token: " + token_value);
    }

    return new_state;
}

// identifier -> (binding, Attribute)
using scope_t = std::unordered_map<std::string, std::pair<std::string,ci_p_t>>;

static bool in_scope(const scope_t & scope, const BindingAttribute & binding_attr) {
    std::string identifier = binding_attr.first + "." + binding_attr.second;
    return scope.count(identifier) > 0;
}

static scope_t construct_scope(Database& db, const SQLParserResult & result) {
    scope_t scope;
    for (auto & rel_pair : result.relations) {
        if (!db.hasTable(rel_pair.first)) {
            throw incorrect_sql_error("unknown relation '" + rel_pair.first + "'");
        }
        auto * table = db.getTable(rel_pair.first);
        size_t attr_cnt = table->getColumnCount();
        for (size_t i = 0; i < attr_cnt; ++i) {
            auto * attr = table->getCI(table->getColumnNames()[i]);
            scope[rel_pair.second + "." + attr->columnName] = std::make_pair(rel_pair.second, attr);
            if (scope.count(attr->columnName) > 0) {
                scope[attr->columnName].second = nullptr;
            } else {
                scope[attr->columnName] = std::make_pair(rel_pair.second, attr);
            }
        }
    }
    return scope;
}

static BindingAttribute fully_qualify(const BindingAttribute & current, const scope_t & scope) {
    if (!current.first.empty()) {
        return current;
    }
    auto it = scope.find(current.second);
    if (it == scope.end()) {
        throw incorrect_sql_error("unknown column '" + current.second + "'");
    }
    return BindingAttribute(it->second.first, current.second);
}

static void fully_qualify_names(const scope_t & scope, SQLParserResult & result) {
    for (size_t i = 0; i < result.selections.size(); ++i) {
        auto & [binding_attr, _] = result.selections[i];
        result.selections[i].first = fully_qualify(binding_attr, scope);
    }

    for (size_t i = 0; i < result.joinConditions.size(); ++i) {
        auto & [lhs, rhs] = result.joinConditions[i];
        result.joinConditions[i].first = fully_qualify(lhs, scope);
        result.joinConditions[i].second = fully_qualify(rhs, scope);
    }

    // TODO same for result.projections
}

static void validate_sql_statement(const scope_t & scope, Database& db, const SQLParserResult & result) {
    for (auto & attr_name : result.projections) {
        auto it = scope.find(attr_name);
        if (it == scope.end()) {
            throw incorrect_sql_error("unknown column '" + attr_name + "'");
        } else if (it->second.second == nullptr) {
            throw incorrect_sql_error("'" + attr_name + "' is ambiguous");
        }
    }

    for (auto & selection_pair : result.selections) {
        auto & binding_attr = selection_pair.first;
        if (!in_scope(scope, binding_attr)) {
            throw incorrect_sql_error("unknown column '" + binding_attr.first + "." + binding_attr.second + "'");
        }
    }

    for (auto & join_pair : result.joinConditions) {
        auto & lhs = join_pair.first;
        if (!in_scope(scope, lhs)) {
            throw incorrect_sql_error("unknown column '" + lhs.first + "." + lhs.second + "'");
        }
        auto & rhs = join_pair.second;
        if (!in_scope(scope, rhs)) {
            throw incorrect_sql_error("unknown column '" + rhs.first + "." + rhs.second + "'");
        }
    }
}

SQLParserResult parse_and_analyse_sql_statement(Database& db, std::string sql) {
    SQLParserResult result;

    std::stringstream in(sql);
    Tokenizer tokenizer(in);

    state_t state = State::Init;
    while (state != State::Done) {
        state = parse_next_token(tokenizer, state, result);
    }

    auto scope = construct_scope(db, result);
    fully_qualify_names(scope, result);
    validate_sql_statement(scope, db, result);
    return result;
}


// insert into versiontable ( vid , tableid , rid ) values ( '3' , 'table' , '5' ) ;
// update versiontable v set vid = 2 where rid = '2';
// update versiontable v set vid = 2 where rid = '5';
// insert into tasks ( rid , user_id , task_name ) values ( 1 , 1 , 'task1' );
// update tasks t set user_id = 2 where rid = 1;
// update tasks t set task_name = 'task2' where rid = 1;
// create table professoren ( name TEXT , rang NUMERIC );
// create table professoren ( name TEXT NOT NULL , rang NUMERIC ( 6 , 2 ) NOT NULL );
// create table professoren ( name VARCHAR ( 15 ) NOT NULL , rang NUMERIC ( 6 , 2 ) NOT NULL );
// INSERT INTO professoren ( name , rang ) VALUES ( 'kemper' , 4 );