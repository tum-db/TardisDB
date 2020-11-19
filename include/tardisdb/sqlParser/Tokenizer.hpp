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
    namespace Keyword {
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

        const std::string Copy = "copy";
        const std::string With = "with";
        const std::string Format = "format";
        const std::string CSV = "csv";
        const std::string TBL = "tbl";

        static std::set<std::string> keywordset = {Version, Select, From, Where, And, Insert, Into, Values, Update, Set, Delete, Create,
                                            Table, Not, Null, Branch, Copy, With, Format, CSV, TBL};
    }

    // Define all control symbols
    namespace controlSymbols {
        const std::string separator = ",";

        const std::string openBracket = "(";
        const std::string closeBracket = ")";

        const std::string star = "*";

        static std::set<std::string> controlSymbolSet = { separator, openBracket, closeBracket, star };
    }

    enum Type : unsigned int {
        delimiter, controlSymbol, keyword, op, identifier, literal
    };

    struct Token {
        Type type;
        std::string value;

        Token(Type type, std::string value) : type(type), value(value) {}

        bool hasType(Type _type) const {
            return type == _type;
        }

        bool equalsKeyword(std::string keywordStr) const {
            return (hasType(keyword) && value.compare(keywordStr) == 0);
        }

        bool equalsControlSymbol(std::string symbolStr) const {
            return (hasType(controlSymbol) && value.compare(symbolStr) == 0);
        }
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
