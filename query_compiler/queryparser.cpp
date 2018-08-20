
#include "query_compiler/queryparser.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace QueryParser {

/*
The select clause contains one or more attribute names. The from clause
contains one or more relation names. The where clause is optional and can
contain one or more selections (connected by "and"). You only need to support
selections of the form "attribute = attribute" and "attribute = constant".
You can assume that each relation is referenced only once, and that all
attribute names are unique.
*/

namespace keyword {
    const std::string Select = "select";
    const std::string From = "from";
    const std::string Where = "where";
    const std::string And = "and";
}

namespace literal {
    const char Comma = ',';
    const char Semicolon = ';';
    const char EqualitySign = '=';
}

typedef enum State : unsigned int {
    Init, Select, AttributeName, AttributeSeperator,
    From, RelationName, RelationSeperator,
    Where, SelectionLhs, SelectionRhs, Equals,
    Constant, And,
    Semicolon
} state_t;

static bool is_identifier(const std::string& str) {
    if (
        str==keyword::Select ||
        str==keyword::From ||
        str==keyword::Where ||
        str==keyword::And
    ) {
        return false;
    }

    return str.find_first_not_of("abcdefghijklmnopqrstuvwxyz_1234567890") == std::string::npos;
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

static state_t next_token(const std::string & token, const state_t state, ParsedQuery & query)
{
    if (token.empty()) {
        return state;
    }

    state_t new_state;
    std::string tok;
    std::transform(token.begin(), token.end(), std::back_inserter(tok), tolower);
    switch(state) {
    case State::Init:
        if (tok == keyword::Select) {
            new_state = State::Select;
        } else {
            throw parser_error("Expected 'Select', found '" + token + "'");
        }
        break;
    case State::Select: /* fallthrough */
    case State::AttributeSeperator:
        if (is_identifier(tok)) {
            query.select.push_back(tok);
            new_state = AttributeName;
        } else {
            throw parser_error("Expected table name, found '" + token + "'");
        }
        break;
    case State::AttributeName:
        if (tok == keyword::From) {
            new_state = State::From;
        } else if (tok.size() == 1 && tok[0] == literal::Comma) {
            new_state = State::AttributeSeperator;
        } else {
            throw parser_error("Expected ',' or 'From' after attribute name, found '" + token + "'");
        }
        break;
    case From: /* fallthrough */
    case RelationSeperator:
        if (is_identifier(tok)) {
            query.from.push_back(tok);
            new_state = RelationName;
        } else {
            throw parser_error("Expected table name, found '" + token + "'");
        }
        break;
    case State::RelationName:
        if (tok == keyword::Where) {
            new_state = State::Where;
        } else if (tok.size() == 1 && tok[0] == literal::Comma) {
            new_state = State::RelationSeperator;
        } else if (tok.size() == 1 && tok[0] == literal::Semicolon) {
            new_state = State::Semicolon;
        } else {
            throw parser_error("Expected ',' or 'Where' after relation name, found '" + token + "'");
        }
        break;
    case State::Where:
    case State::And: /* fallthrough */
        if (is_identifier(tok)) {
//            const auto attr = SchemaParser::getAttribute(tok);
//            query.selections.push_back( std::make_unique<Query::Selection>(attr) );
            query.where.emplace_back(tok, ParsedQuery::Predicate::Attribute);
            new_state = State::SelectionLhs;
        } else {
            throw parser_error("Expected attribute name, found '" + token + "'");
        }
        break;
    case State::SelectionLhs:
        if (tok.size() == 1 && tok[0] == literal::EqualitySign) {
            new_state = State::Equals;
        } else {
            throw parser_error("Expected '=', found '" + token + "'");
        }
        break;
    case State::Equals: {
        auto & predicate = query.where.back();
        if (is_escaped(tok)) {
            std::string constant = unescape(token);
//            query.selections.back()->setSecond(constant);
            predicate.rhs = constant;
            predicate.rhsType = ParsedQuery::Predicate::Constant;

            new_state = State::SelectionRhs;
        } else if (tok.size() > 0 && std::isdigit(tok[0])) {
//            query.selections.back()->setSecond(token);
            predicate.rhs = token;
            predicate.rhsType = ParsedQuery::Predicate::Constant;

            new_state = State::SelectionRhs;
        } else if (is_identifier(tok)) {
//            const auto attr = SchemaParser::getAttribute(tok);
//            query.selections.back()->setSecond(attr);
            predicate.rhs = token;
            predicate.rhsType = ParsedQuery::Predicate::Attribute;

            new_state = State::SelectionRhs;
        } else {
            throw parser_error("Expected attribute name or constant, found '" + token + "'");
        }
        break;
    }
    case State::SelectionRhs:
        if (tok == keyword::And) {
            new_state = State::And;
        } else if (tok.size() == 1 && tok[0] == literal::Semicolon) {
            new_state = State::Semicolon;
        } else {
            throw parser_error("Expected 'And' or semicolon, found '" + token + "'");
        }
        break;
    case State::Semicolon: /* fallthrough */
    default:
        throw std::runtime_error("unexpected token: " + token);
    }

    return new_state;
}

std::unique_ptr<ParsedQuery> parse_query(const std::string & query)
{
    std::unique_ptr<ParsedQuery> q = std::make_unique<ParsedQuery>();
    std::stringstream in(query);

    std::string token;
    state_t state = State::Init;

    while (in >> token) {
        std::string::size_type pos;
        std::string::size_type prevPos = 0;

        while ((pos = token.find_first_of(",;", prevPos)) != std::string::npos) {
            state = next_token(token.substr(prevPos, pos-prevPos), state, *q);
            state = next_token(token.substr(pos, 1), state, *q);
            prevPos = pos + 1;
        }

        state = next_token(token.substr(prevPos), state, *q);
    }

    if (
        state != State::RelationName &&
        state != State::SelectionRhs &&
        state != State::Semicolon
    ) {
        throw parser_error("unexpected end of input");
    }

    return q;
}

} // end namespace QueryParser
