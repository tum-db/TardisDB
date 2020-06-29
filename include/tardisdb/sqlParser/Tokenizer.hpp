//
// Created by Blum Thomas on 2020-06-27.
//

#ifndef PROTODB_TOKENIZER_HPP
#define PROTODB_TOKENIZER_HPP

#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace tardisParser {
    // Define all SQL keywords
    namespace keywords {
        const std::string Version = "version";

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

        const std::string Branch = "branch";

        static std::set<std::string> keywordset = {Version, Select, From, Where, And, Insert, Into, Values, Update, Set, Delete, Create,
                                            Table, Not, Null, Branch};
    }

    // Define all control symbols
    namespace controlSymbols {
        const std::string separator = ",";

        const std::string openBracket = "(";
        const std::string closeBracket = ")";

        const std::string star = "*";

        static std::set<std::string> controlSymbolSet = { separator, openBracket, closeBracket, star };
    }

    enum TokenType : unsigned int {
        delimiter, controlSymbol, keyword, op, identifier, literal
    };

    struct Token {
        TokenType type;
        std::string value;

        Token(TokenType type, std::string value) : type(type), value(value) {}
    };

    class Tokenizer {
    public:

        Tokenizer(std::istream &in) : _in(in) {}

        const Token &prev(size_t i = 1);

        const Token &next();

    private:
        std::istream &_in;
        std::vector<Token> _tokens;

        static bool is_delimiter(Token &tok);

        static bool is_controlSymbol(Token &tok);

        static bool is_keyword(Token &tok);

        static bool is_op(Token &tok);

        static bool is_identifier(Token &tok);

        static bool is_escaped(const std::string &str);

        static std::string unescape(const std::string &str);
    };
}


#endif //PROTODB_TOKENIZER_HPP
