#ifndef PROTODB_SQLPARSER_HPP
#define PROTODB_SQLPARSER_HPP

#include <string>

#include "sqlParser/Tokenizer.hpp"
#include "sqlParser/ParserResult.hpp"

namespace tardisParser {
    struct syntactical_error : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    class SQLParser {
    public:

        static void parseStatement(ParsingContext &context, std::string statement);

    private:

        static BindingAttribute parse_binding_attribute(std::string value);

        static void parseToken(Tokenizer &tokenizer, ParsingContext &context);
    };
}

#endif //PROTODB_SQLPARSER_HPP