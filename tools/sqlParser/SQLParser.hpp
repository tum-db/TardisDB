#ifndef PROTODB_SQLPARSER_HPP
#define PROTODB_SQLPARSER_HPP

#include <string>

#include "sqlParser/Tokenizer.hpp"
#include "sqlParser/ParserResult.hpp"
#include "sqlParser/Expression.hpp"

namespace tardisParser {
    struct syntactical_error : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    class SQLParser {
    public:

        static void parseStatement(ParsingContext &context, std::string statement);

        /// \returns The tree of an expression, which must make up all of \p text
        static sqlExpressions::Node parseExpression(const std::string & text);

    private:

        static BindingAttribute parse_binding_attribute(std::string value);

        static void parseToken(Tokenizer &tokenizer, ParsingContext &context);
    };
}

#endif //PROTODB_SQLPARSER_HPP