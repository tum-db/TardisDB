//
// Created by Blum Thomas on 2020-06-27.
//

#include <stdexcept>
#include <algorithm>
#include <cassert>

#include "sqlParser/Tokenizer.hpp"

namespace tardisParser {
    // Recognizes if token is a delimiter
    bool Tokenizer::is_delimiter(Token &tok) {
        if (tok.type == Type::delimiter) return true;
        if (tok.value.compare(";") == 0) {
            tok.type = Type::delimiter;
            return true;
        }
        return false;
    }

    // Recognizes if token is a control symbol
    bool Tokenizer::is_controlSymbol(Token &tok) {
        if (tok.type == Type::delimiter) return false;
        if (tok.type == Type::controlSymbol) return true;
        if (controlSymbols::controlSymbolSet.count(tok.value)) {
            tok.type = Type::controlSymbol;
            return true;
        }
        return false;
    }

    // Recognizes if token is a sql keyword
    bool Tokenizer::is_keyword(Token &tok) {
        if (tok.type == Type::delimiter || tok.type == Type::controlSymbol) return false;
        if (tok.type == Type::keyword) return true;
        std::string lowercase_token_value;
        std::transform(tok.value.begin(), tok.value.end(), std::back_inserter(lowercase_token_value), ::tolower);

        if (Keyword::keywordset.count(lowercase_token_value)) {
            tok.type = keyword;
            tok.value = lowercase_token_value;
            return true;
        }
        return false;
    }

    // Recognizes if token is a operator
    bool Tokenizer::is_op(Token &tok) {
        if (tok.type == Type::delimiter || tok.type == Type::controlSymbol ||
            tok.type == Type::keyword)
            return false;
        if (tok.type == Type::op) return true;
        static const std::set<std::string> comparisons = {"=", "<", "<=", ">", ">=", "<>", "!="};
        if (comparisons.count(tok.value) > 0) {
            tok.type = Type::op;
            return true;
        }
        return false;
    }

    // Recognizes if token is a identifier
    bool Tokenizer::is_identifier(Token &tok) {
        if (tok.type == Type::delimiter || tok.type == Type::controlSymbol ||
            tok.type == Type::keyword || tok.type == Type::op)
            return false;
        if (tok.type == Type::identifier) return true;
        std::string lowercase_token_value;
        std::transform(tok.value.begin(), tok.value.end(), std::back_inserter(lowercase_token_value), ::tolower);
        if (!std::isdigit(tok.value[0]) &&
            tok.value[0] != '.' &&
            tok.value[0] != '_' &&
            lowercase_token_value.find_first_not_of("abcdefghijklmnopqrstuvwxyz_.1234567890") == std::string::npos) {
            tok.type = Type::identifier;
            return true;
        }
        return false;
    }

    // Recognizes if literal is escaped
    bool Tokenizer::is_escaped(const std::string &str) {
        if (str.length() > 2 && str.front() == '"') {
            return (str.back() == '"');
        } else if (str.length() > 2 && str.front() == '\'') {
            return (str.back() == '\'');
        }
        return false;
    }

    // Unescapees literal
    std::string Tokenizer::unescape(const std::string &str) {
        if (is_escaped(str)) {
            return str.substr(1, str.size() - 2);
        }
        return str;
    }

    const Token &Tokenizer::prev(size_t i) {
        assert(_tokens.size() >= i);
        return _tokens[_tokens.size() - 1 - i];
    }

    // Skips a PostgreSQL cast (::type, ::numeric(10,2)) behind a string literal: TardisDB casts every
    // literal to the type of the column it is compared with or stored in
    void Tokenizer::skipCast() {
        while (std::isspace(_in.peek())) _in.get();
        if (_in.peek() != ':') return;
        _in.get();
        if (_in.peek() != ':') { _in.putback(':'); return; }
        _in.get();
        while (std::isalnum(_in.peek()) || _in.peek() == '_') _in.get();
        if (_in.peek() == '(') {
            while (_in.peek() != EOF && _in.get() != ')') { }
        }
    }

    const Token &Tokenizer::next() {
        std::stringstream ss;

        // a string literal is one token, whatever it contains; '' stands for a quote
        while (std::isspace(_in.peek())) _in.get();
        if (_in.peek() == '\'') {
            _in.get();
            std::string value;
            for (int c = _in.get(); ; c = _in.get()) {
                if (c == EOF) throw std::runtime_error("unterminated string literal");
                if (c == '\'') {
                    if (_in.peek() != '\'') break;
                    _in.get();
                }
                value += static_cast<char>(c);
            }
            skipCast();
            _tokens.emplace_back(Type::literal, value);
            return _tokens.back();
        }

        size_t i = 0;   // Token length
        while (true) {
            // Get character
            int c = _in.get();

            // Check character if special
            if (c == EOF) {
                // If stream reaches end without terminating with a delimiter put a delimiter back
                _in.putback(';');
                if (i > 0) break;
            } else if (std::isspace(c)) {
                if (i > 0) {
                    // If some token characters have already been read a space marks the end
                    break;
                } else {
                    // If no token character has been read so far discard the spaces until one appears
                    continue;
                }
            } else if (c == '<' || c == '>' || c == '=' || c == '!') {
                // a comparison ends the token before it: a>=5 is a, >=, 5
                if (i > 0) {
                    _in.putback(c);
                } else {
                    ss << static_cast<char>(c);
                    int n = _in.peek();
                    if ((c == '<' && (n == '=' || n == '>')) || ((c == '>' || c == '!') && n == '=')) {
                        ss << static_cast<char>(_in.get());
                    }
                }
                break;
            } else if (c == ';' || controlSymbols::controlSymbolSet.count(std::string({static_cast<char>(c)})) > 0) {
                if (i > 0) {
                    // If it is a control symbol and some token characters habe been read so far
                    // the control symbol is a new token and has to be put back and the current token terminates
                    _in.putback(c);
                } else {
                    // If no token character has been read so far the control symbol is the current token
                    // and the scan can be stopped
                    ss << static_cast<char>(c);
                }
                break;
            }
            // If the character is no special character add it to the token
            ss << static_cast<char>(c);
            i++;
        }

        // Check if the token is a special kind of token. If not it is a literal.
        std::string value = ss.str();
        auto cast = value.find("::"); // 5::integer, as for string literals
        if (cast != std::string::npos && cast > 0) value = value.substr(0, cast);
        Token token = Token(Type::literal, value);
        std::string lowercase = value;
        std::transform(lowercase.begin(), lowercase.end(), lowercase.begin(), ::tolower);
        if (lowercase == "true" || lowercase == "false") {
            // a boolean constant
            token.value = lowercase;
            _tokens.emplace_back(token);
            return _tokens.back();
        }
        if (!is_delimiter(token) && !is_controlSymbol(token) && !is_keyword(token) && !is_op(token) &&
            !is_identifier(token)) {
            token.value = unescape(token.value);
        }

        _tokens.emplace_back(token);

        return _tokens.back();
    }
}
