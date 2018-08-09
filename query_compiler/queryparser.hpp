
#ifndef QUERYPARSER_HPP
#define QUERYPARSER_HPP

#include <string>

#include "schema_parser/Schema.hpp"

namespace QueryParser {

struct ParsedQuery {
    struct Predicate {
        enum TokenType { Attribute, Constant };

        std::string lhs;
        TokenType lhsType;
        std::string rhs;
        TokenType rhsType;

        Predicate(std::string lhs, TokenType lhsType) :
                lhs(lhs), lhsType(lhsType)
        { }
    };

    std::vector<std::string> select;
    std::vector<std::string> from;
    std::vector<Predicate> where;
};

class parser_error : std::exception {
public:
    parser_error(const std::string & m) : msg(m) { }
    ~parser_error() throw() { }
    const char * what() const throw() {
        return msg.c_str();
    }

private:
    std::string msg;
};

std::unique_ptr<ParsedQuery> parse_query(const std::string & query);

} // end namespace QueryParser

#endif // QUERYPARSER_HPP
