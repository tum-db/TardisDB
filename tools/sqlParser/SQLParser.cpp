#include <algorithm>
#include <functional>

#include "sqlParser/SQLParser.hpp"
#include "native/sql/SqlValues.hpp"

namespace tardisParser {
    static bool isArithmeticOperator(const Token &token) {
        return (token.hasType(Type::literal) && (token.value == "+" || token.value == "-" || token.value == "/")) ||
               token.equalsControlSymbol(controlSymbols::star);
    }

    static bool equalsIgnoreCase(const std::string &a, const std::string &b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
                                                  [](char x, char y) { return ::tolower(x) == ::tolower(y); });
    }

    // Parses an identifier into a binding and its attribute
    BindingAttribute SQLParser::parse_binding_attribute(std::string value) {
        auto dotPos = value.find_first_of(".");
        if (dotPos == std::string::npos) {
            // throw incorrect_sql_error("invalid binding attribute");
            return BindingAttribute({}, value);
        }

        auto binding = value.substr(0, dotPos);
        auto attribute = value.substr(dotPos + 1, value.size());
        return BindingAttribute(binding, attribute);
    }

    /// binding.attribute or attribute
    static Column columnOf(const std::string & value) {
        Column column;
        auto dot = value.find('.');
        column.table = (dot == std::string::npos) ? "" : value.substr(0, dot);
        column.name = (dot == std::string::npos) ? value : value.substr(dot + 1);
        return column;
    }

    static std::string flipped(const std::string & op) {
        if (op == "<") return ">";
        if (op == "<=") return ">=";
        if (op == ">") return "<";
        if (op == ">=") return "<=";
        return op;
    }

    /// \returns The token, or the constant NULL (Native::Sql::nullLiteral) for the keyword NULL
    static Token valueToken(const Token & token) {
        if (token.equalsKeyword(Keyword::Null)) return Token(Type::literal, Native::Sql::nullLiteral);
        return token;
    }

    /// Reads ( a , b , ... ) after the opening bracket: identifiers or literals
    static std::vector<Token> readList(Tokenizer & tokenizer) {
        std::vector<Token> items;
        for (;;) {
            Token item = valueToken(tokenizer.next());
            if (item.type != Type::identifier && item.type != Type::literal) {
                throw syntactical_error("Expected a column or a value, found '" + item.value + "'");
            }
            items.push_back(item);
            Token next = tokenizer.next();
            if (next.equalsControlSymbol(controlSymbols::closeBracket)) return items;
            if (!next.equalsControlSymbol(controlSymbols::separator)) {
                throw syntactical_error("Expected ',' or ')', found '" + next.value + "'");
            }
        }
    }

    using sqlExpressions::Node;

    static bool isWordToken(const Token & token, const char * word) {
        if (token.type != Type::identifier && token.type != Type::keyword) return false;
        std::string value = token.value;
        std::transform(value.begin(), value.end(), value.begin(), ::tolower);
        return value == word;
    }

    /// Reads an expression by precedence (as PostgreSQL: OR, AND, NOT, comparisons with IS, BETWEEN and
    /// IN, the other operators || @> <@ && ** and prefix @, + and -, * and /, ^, unary minus, subscripts);
    /// token() is then the first token after it
    class ExpressionParser {
    public:
        ExpressionParser(Tokenizer & tokenizer, Token first) :
                _tokenizer(tokenizer), _token(valueToken(first)), _keptCasts(tokenizer.keepCasts(true)) { }

        ~ExpressionParser() { _tokenizer.keepCasts(_keptCasts); }

        Node parse() { return disjunction(); }

        const Token & token() const { return _token; }

    private:
        Tokenizer & _tokenizer;
        Token _token;
        bool _keptCasts;

        void advance() { _token = valueToken(_tokenizer.next()); }
        bool word(const char * w) const { return isWordToken(_token, w); }
        bool symbol(const std::string & s) const { return _token.equalsControlSymbol(s); }
        bool bare(const char * v) const { return _token.type == Type::literal && !_token.quoted && _token.value == v; }
        void expect(const std::string & s) {
            if (!symbol(s)) throw syntactical_error("Expected '" + s + "', found '" + _token.value + "'");
            advance();
        }

        static Node make(Node::Kind kind, const std::string & name, std::vector<Node> children) {
            Node n;
            n.kind = kind;
            n.name = name;
            n.children = std::move(children);
            return n;
        }

        Node disjunction() {
            Node l = conjunction();
            while (word("or")) {
                advance();
                l = make(Node::Kind::Operator, "or", {std::move(l), conjunction()});
            }
            return l;
        }

        Node conjunction() {
            Node l = negation();
            while (_token.equalsKeyword(Keyword::And)) {
                advance();
                l = make(Node::Kind::Operator, "and", {std::move(l), negation()});
            }
            return l;
        }

        Node negation() {
            if (_token.equalsKeyword(Keyword::Not)) {
                advance();
                return make(Node::Kind::Operator, "not", {negation()});
            }
            return comparison();
        }

        static bool isComparison(const std::string & op) {
            return op == "=" || op == "<>" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=";
        }

    public:
        static bool isComparisonName(const std::string & op) { return isComparison(op); }

    private:

        Node comparison() {
            Node l = other();
            for (;;) {
                if (_token.type == Type::op && isComparison(_token.value)) {
                    std::string op = _token.value == "!=" ? "<>" : _token.value;
                    advance();
                    if (word("any") || word("some") || word("all")) {
                        Node quantified = make(Node::Kind::Any, op, {});
                        quantified.value = word("all") ? "all" : "any";
                        advance();
                        expect(controlSymbols::openBracket);
                        Node array = disjunction();
                        expect(controlSymbols::closeBracket);
                        quantified.children = {std::move(l), std::move(array)};
                        l = std::move(quantified);
                        continue;
                    }
                    l = make(Node::Kind::Operator, op, {std::move(l), other()});
                    continue;
                }
                if (word("is")) {
                    advance();
                    bool negated = _token.equalsKeyword(Keyword::Not);
                    if (negated) advance();
                    if (!_token.equalsKeyword(Keyword::Null) && !(_token.type == Type::literal && _token.value == Native::Sql::nullLiteral)) {
                        throw syntactical_error("Expected IS [ NOT ] NULL, found '" + _token.value + "'");
                    }
                    advance();
                    l = make(Node::Kind::Operator, negated ? "is not null" : "is null", {std::move(l)});
                    continue;
                }
                bool negated = false;
                if (_token.equalsKeyword(Keyword::Not)) {
                    // after an operand: NOT IN or NOT BETWEEN
                    advance();
                    negated = true;
                    if (!word("in") && !word("between")) throw syntactical_error("Expected IN or BETWEEN after NOT, found '" + _token.value + "'");
                }
                if (word("between")) {
                    advance();
                    Node low = other();
                    if (!_token.equalsKeyword(Keyword::And)) throw syntactical_error("Expected BETWEEN value AND value");
                    advance();
                    l = make(Node::Kind::Between, "between", {std::move(l), std::move(low), other()});
                } else if (word("in")) {
                    advance();
                    expect(controlSymbols::openBracket);
                    std::vector<Node> items;
                    items.push_back(std::move(l));
                    for (;;) {
                        items.push_back(disjunction());
                        if (symbol(controlSymbols::separator)) { advance(); continue; }
                        expect(controlSymbols::closeBracket);
                        break;
                    }
                    l = make(Node::Kind::In, "in", std::move(items));
                } else {
                    break;
                }
                if (negated) l = make(Node::Kind::Operator, "not", {std::move(l)});
            }
            return l;
        }

        Node other() {
            Node l = prefixOther();
            while (_token.type == Type::op && (_token.value == "||" || _token.value == "@>" || _token.value == "<@" ||
                                               _token.value == "&&" || _token.value == "**")) {
                std::string op = _token.value;
                advance();
                l = make(Node::Kind::Operator, op, {std::move(l), prefixOther()});
            }
            return l;
        }

        /// @ x (absolute values): a prefix operator of the precedence of the other operators
        Node prefixOther() {
            if (_token.type == Type::op && _token.value == "@") {
                advance();
                return make(Node::Kind::Operator, "@", {prefixOther()});
            }
            return additive();
        }

        Node additive() {
            Node l = multiplicative();
            while (bare("+") || bare("-")) {
                std::string op = _token.value;
                advance();
                l = make(Node::Kind::Operator, op, {std::move(l), multiplicative()});
            }
            return l;
        }

        Node multiplicative() {
            Node l = power();
            while (symbol(controlSymbols::star) || bare("/")) {
                std::string op = symbol(controlSymbols::star) ? "*" : "/";
                advance();
                l = make(Node::Kind::Operator, op, {std::move(l), power()});
            }
            return l;
        }

        /// x ^ y, from left to right; unary minus binds closer (-2 ^ 2 is 4), as in PostgreSQL
        Node power() {
            Node l = unary();
            while (_token.type == Type::op && _token.value == "^") {
                advance();
                l = make(Node::Kind::Operator, "^", {std::move(l), unary()});
            }
            return l;
        }

        Node unary() {
            if (bare("+")) {
                advance();
                return unary();
            }
            if (bare("-")) {
                advance();
                Node operand = unary();
                if (operand.kind == Node::Kind::Literal && !operand.quoted && !operand.value.empty() &&
                    (std::isdigit(static_cast<unsigned char>(operand.value[0])) || operand.value[0] == '.')) {
                    operand.value = "-" + operand.value; // a negative number
                    return operand;
                }
                return make(Node::Kind::Operator, "neg", {std::move(operand)});
            }
            return postfix();
        }

        /// A type of a cast: a name (double precision, character varying), (length, precision), [] or ARRAY
        Node castTo(Node value, std::string type) {
            Node cast = make(Node::Kind::Cast, "", {std::move(value)});
            auto open = type.find('(');
            if (open != std::string::npos) {
                auto close = type.find(')', open);
                cast.value = type.substr(open + 1, close - open - 1);
                type = type.substr(0, open) + type.substr(close + 1);
            }
            cast.name = type;
            return cast;
        }

        std::string typeName() {
            if (_token.type != Type::identifier && _token.type != Type::keyword) throw syntactical_error("Expected a type, found '" + _token.value + "'");
            std::string type;
            std::transform(_token.value.begin(), _token.value.end(), std::back_inserter(type), ::tolower);
            advance();
            if (type == "double" && word("precision")) advance();
            if (type == "character" && word("varying")) { type = "varchar"; advance(); }
            if (symbol(controlSymbols::openBracket)) {
                advance();
                type += "(" + _token.value;
                advance();
                if (symbol(controlSymbols::separator)) {
                    advance();
                    type += "," + _token.value;
                    advance();
                }
                expect(controlSymbols::closeBracket);
                type += ")";
            }
            bool array = false;
            for (;;) {
                if (symbol(controlSymbols::openSquare)) {
                    advance();
                    if (_token.type == Type::literal) advance();
                    expect(controlSymbols::closeSquare);
                    array = true;
                } else if (word("array")) {
                    advance();
                    array = true;
                } else {
                    break;
                }
            }
            return array ? type + "[]" : type;
        }

        Node postfix() {
            Node n = primary();
            for (;;) {
                if (_token.type == Type::op && _token.value == "::") {
                    advance();
                    n = castTo(std::move(n), typeName());
                    continue;
                }
                if (!symbol(controlSymbols::openSquare)) break;
                advance();
                Node index;
                Node omitted = make(Node::Kind::Omitted, "", {});
                bool slice = false;
                Node lower = omitted, upper = omitted;
                if (!bare(":")) lower = disjunction();
                if (bare(":")) {
                    slice = true;
                    advance();
                    if (!symbol(controlSymbols::closeSquare)) upper = disjunction();
                }
                expect(controlSymbols::closeSquare);
                index = slice ? make(Node::Kind::Slice, ":", {std::move(lower), std::move(upper)}) : std::move(lower);
                if (n.kind != Node::Kind::Subscript) n = make(Node::Kind::Subscript, "[]", {std::move(n)});
                n.children.push_back(std::move(index));
            }
            return n;
        }

        /// ARRAY[ ... ] after its [, or a [ ... ] in it
        Node arrayConstructor() {
            Node array = make(Node::Kind::Array, "array", {});
            if (symbol(controlSymbols::closeSquare)) {
                advance();
                return array;
            }
            for (;;) {
                if (symbol(controlSymbols::openSquare)) {
                    advance();
                    array.children.push_back(arrayConstructor());
                } else {
                    array.children.push_back(disjunction());
                }
                if (symbol(controlSymbols::separator)) { advance(); continue; }
                expect(controlSymbols::closeSquare);
                return array;
            }
        }

        Node primary() {
            if (symbol(controlSymbols::openBracket)) {
                advance();
                std::vector<Node> items;
                items.push_back(disjunction());
                while (symbol(controlSymbols::separator)) {
                    advance();
                    items.push_back(disjunction());
                }
                expect(controlSymbols::closeBracket);
                if (items.size() == 1) return std::move(items.front());
                return make(Node::Kind::Row, "row", std::move(items));
            }
            if (_token.type == Type::literal && _token.value != ":" && !(bare("+") || bare("-") || bare("/"))) {
                Node literal = make(Node::Kind::Literal, "", {});
                literal.value = _token.value;
                literal.quoted = _token.quoted;
                std::string cast = _token.cast; // skipped before the expression began
                advance();
                if (!cast.empty()) return castTo(std::move(literal), cast);
                return literal;
            }
            if (_token.type == Type::identifier) {
                Token name = _token;
                advance();
                if (isWordToken(name, "array") && symbol(controlSymbols::openSquare)) {
                    advance();
                    return arrayConstructor();
                }
                if (isWordToken(name, "cast") && symbol(controlSymbols::openBracket)) {
                    // CAST ( value AS type )
                    advance();
                    Node value = disjunction();
                    if (!word("as")) throw syntactical_error("Expected AS, found '" + _token.value + "'");
                    advance();
                    Node cast = castTo(std::move(value), typeName());
                    expect(controlSymbols::closeBracket);
                    return cast;
                }
                if (symbol(controlSymbols::openBracket)) {
                    // a call
                    advance();
                    Node call = make(Node::Kind::Function, "", {});
                    std::transform(name.value.begin(), name.value.end(), std::back_inserter(call.name), ::tolower);
                    if (symbol(controlSymbols::star)) {
                        call.value = "*";
                        advance();
                        expect(controlSymbols::closeBracket);
                        return call;
                    }
                    if (symbol(controlSymbols::closeBracket)) {
                        advance();
                        return call;
                    }
                    for (;;) {
                        call.children.push_back(disjunction());
                        if (symbol(controlSymbols::separator)) { advance(); continue; }
                        expect(controlSymbols::closeBracket);
                        return call;
                    }
                }
                Column column = columnOf(name.value);
                Node n = make(Node::Kind::Column, column.name, {});
                n.table = column.table;
                return n;
            }
            throw syntactical_error("Expected an expression, found '" + _token.value + "'");
        }
    };

    /// A constant without its cast ('5'::integer is 5, as TardisDB types constants by their context)
    static const Node & uncast(const Node & n) {
        if (n.kind == Node::Kind::Cast && n.children.front().kind == Node::Kind::Literal) return n.children.front();
        return n;
    }

    /// The text of an ARRAY[ ... ] of constants ('{"1","2"}'), as INSERT takes constants
    static std::string arrayLiteral(const Node & array) {
        std::string out = "{";
        for (size_t i = 0; i < array.children.size(); ++i) {
            const Node & e = uncast(array.children[i]);
            if (i > 0) out += ",";
            if (e.kind == Node::Kind::Array) {
                out += arrayLiteral(e);
            } else if (e.kind == Node::Kind::Literal && e.value == Native::Sql::nullLiteral) {
                out += "NULL";
            } else if (e.kind == Node::Kind::Literal) {
                out += '"';
                for (char c : e.value) {
                    if (c == '"' || c == '\\') out += '\\';
                    out += c;
                }
                out += '"';
            } else {
                throw syntactical_error("ARRAY[ ... ] takes constants here");
            }
        }
        return out + "}";
    }

    /// The name of a computed column, as PostgreSQL names it: the function, the column of a subscript,
    /// "array", otherwise ?column?
    static std::string outputName(const Node & e) {
        switch (e.kind) {
            case Node::Kind::Function: return e.name;
            case Node::Kind::Subscript: return e.children.front().kind == Node::Kind::Column ? e.children.front().name : outputName(e.children.front());
            case Node::Kind::Array: return "array";
            case Node::Kind::Column: return e.name;
            default: return "?column?";
        }
    }

    /// Parses the conditions of WHERE, beginning with \p first, up to the end of the statement or, if
    /// \p groupMayFollow, to GROUP or HAVING: any condition (see ExpressionParser). Of the conjuncts (AND), the
    /// comparisons with =, <>, <, <=, > and >= of a column and a value (either side), of two row values
    /// ( a , b ) >= ( 1 , 2 ), column BETWEEN v AND w, column IN ( v , ... ), column IS [ NOT ] NULL and,
    /// given \p joins, the equalities of two columns are kept apart, which index probes, joins and the
    /// queries over several branches take: equalities of a column and a value become selections, the
    /// others predicates. All other conjuncts are predicates of op "expr". A comparison with NULL is NULL,
    /// so it holds for no tuple.
    /// \returns The keyword that ended the conditions: GROUP, HAVING or none (the end)
    enum class WhereEnd { End, Group, Having };
    static WhereEnd parseWhere(Tokenizer & tokenizer, Token token, std::vector<std::pair<Column,std::string>> & selections,
                               std::vector<Predicate> & predicates, std::vector<std::pair<Column,Column>> * joins,
                               bool groupMayFollow) {
        ExpressionParser parser(tokenizer, token);
        Node condition = parser.parse();
        const Token & end = parser.token();
        WhereEnd group = WhereEnd::End;
        if (groupMayFollow && end.equalsKeyword(Keyword::Group)) {
            group = WhereEnd::Group;
        } else if (groupMayFollow && end.equalsKeyword(Keyword::Having)) {
            group = WhereEnd::Having;
        } else if (!end.hasType(Type::delimiter)) {
            throw syntactical_error("Expected 'AND'" + std::string(groupMayFollow ? ", 'GROUP', 'HAVING'" : "") + " or the end, found '" + end.value + "'");
        }

        std::vector<const Node *> conjuncts;
        std::function<void(const Node &)> split = [&](const Node & n) {
            if (n.kind == Node::Kind::Operator && n.name == "and") {
                split(n.children[0]);
                split(n.children[1]);
            } else {
                conjuncts.push_back(&n);
            }
        };
        split(condition);

        auto isColumn = [](const Node & n) { return n.kind == Node::Kind::Column; };
        // a constant of the predicates, which take it in the column's type; numbers with a point or an
        // exponent go through the expression instead, which compares in the common type (k < 1.5 of an integer k)
        auto isValue = [](const Node & n) {
            const Node & u = uncast(n);
            if (u.kind != Node::Kind::Literal) return false;
            const bool number = !u.quoted && !u.value.empty() && (std::isdigit(static_cast<unsigned char>(u.value[0])) ||
                                                                   u.value[0] == '-' || u.value[0] == '+' || u.value[0] == '.');
            return !number || u.value.find_first_of(".eE") == std::string::npos;
        };
        auto items = [](const Node & n) {
            std::vector<const Node *> list;
            if (n.kind == Node::Kind::Row) for (auto & c : n.children) list.push_back(&uncast(c));
            else list.push_back(&uncast(n));
            return list;
        };
        auto all = [](const std::vector<const Node *> & list, auto predicate) {
            return std::all_of(list.begin(), list.end(), [&](const Node * n) { return predicate(*n); });
        };
        auto columnOfNode = [](const Node & n) {
            Column column;
            column.name = n.name;
            column.table = n.table;
            return column;
        };
        auto generic = [&](const Node & n) {
            Predicate predicate;
            predicate.op = "expr";
            predicate.expression = std::make_shared<Node>(n);
            predicates.push_back(std::move(predicate));
        };
        for (const Node * c : conjuncts) {
            if (c->kind == Node::Kind::Operator && c->children.size() == 2 && ExpressionParser::isComparisonName(c->name)) {
                std::string op = c->name;
                auto lhs = items(c->children[0]), rhs = items(c->children[1]);
                if (lhs.size() != rhs.size()) throw syntactical_error("row values of different lengths");
                if (all(lhs, isValue) && all(rhs, isColumn)) {
                    std::swap(lhs, rhs);
                    op = flipped(op);
                }
                if (all(lhs, isColumn) && all(rhs, isColumn) && joins != nullptr && op == "=" && lhs.size() == 1) {
                    joins->emplace_back(columnOfNode(*lhs[0]), columnOfNode(*rhs[0]));
                    continue;
                }
                if (all(lhs, isColumn) && all(rhs, isValue)) {
                    if (op == "=") {
                        for (size_t i = 0; i < lhs.size(); ++i) selections.emplace_back(columnOfNode(*lhs[i]), rhs[i]->value);
                        continue;
                    }
                    Predicate predicate;
                    for (auto * n : lhs) predicate.columns.push_back(columnOfNode(*n));
                    predicate.op = op;
                    for (auto * n : rhs) predicate.values.push_back(n->value);
                    predicates.push_back(std::move(predicate));
                    continue;
                }
                if (lhs.size() > 1) throw syntactical_error("a comparison of row values needs columns on one side and values on the other");
            } else if (c->kind == Node::Kind::Between && isColumn(c->children[0]) && isValue(c->children[1]) && isValue(c->children[2])) {
                for (auto [op, bound] : {std::make_pair(">=", 1), std::make_pair("<=", 2)}) {
                    Predicate predicate;
                    predicate.columns.push_back(columnOfNode(c->children[0]));
                    predicate.op = op;
                    predicate.values.push_back(uncast(c->children[bound]).value);
                    predicates.push_back(std::move(predicate));
                }
                continue;
            } else if (c->kind == Node::Kind::In && isColumn(c->children[0]) &&
                       std::all_of(c->children.begin() + 1, c->children.end(), isValue)) {
                Predicate predicate;
                predicate.columns.push_back(columnOfNode(c->children[0]));
                predicate.op = "in";
                for (size_t i = 1; i < c->children.size(); ++i) predicate.values.push_back(uncast(c->children[i]).value);
                predicates.push_back(std::move(predicate));
                continue;
            } else if (c->kind == Node::Kind::Operator && (c->name == "is null" || c->name == "is not null") && isColumn(c->children[0])) {
                Predicate predicate;
                predicate.columns.push_back(columnOfNode(c->children[0]));
                predicate.op = c->name;
                predicates.push_back(std::move(predicate));
                continue;
            }
            generic(*c);
        }
        return group;
    }

    /// The terms of SET c = t1 op t2 ... (constants and columns with + - * /, '*' and '/' binding first), if
    /// the expression is of them without parentheses that change the order
    static bool toTerms(const Node & e, std::vector<UpdateTerm> & terms) {
        auto leaf = [](const Node & n) { return n.kind == Node::Kind::Column || uncast(n).kind == Node::Kind::Literal; };
        std::function<bool(const Node &, bool)> fits = [&](const Node & n, bool sum) {
            if (leaf(n)) return true;
            if (n.kind != Node::Kind::Operator || n.children.size() != 2) return false;
            if (n.name == "*" || n.name == "/") return fits(n.children[0], false) && leaf(n.children[1]);
            if (sum && (n.name == "+" || n.name == "-")) return fits(n.children[0], true) && fits(n.children[1], false);
            return false;
        };
        if (!fits(e, true)) return false;
        std::function<void(const Node &, const std::string &)> flatten = [&](const Node & n, const std::string & op) {
            if (leaf(n)) {
                UpdateTerm term;
                term.op = op;
                if (n.kind == Node::Kind::Column) {
                    term.column.name = n.name;
                    term.column.table = n.table;
                } else {
                    term.value = uncast(n).value;
                }
                terms.push_back(term);
                return;
            }
            flatten(n.children[0], op);
            flatten(n.children[1], n.name);
        };
        flatten(e, "");
        return true;
    }

    static bool isWord(const Token & token, const std::string & word) {
        std::string value = token.value;
        std::transform(value.begin(), value.end(), value.begin(), ::tolower);
        return (token.type == Type::identifier || token.type == Type::keyword) && value == word;
    }

    /// Reads what may follow the name of a type: [] (with an ignored size, as the dimensions, which
    /// PostgreSQL does not enforce either) or ARRAY make it an array type ("[]" at the end of \p type),
    /// PRECISION completes DOUBLE, VARYING CHARACTER
    /// \returns Whether the token belonged to the type
    static bool typeSuffix(Tokenizer & tokenizer, const Token & token, std::string & type) {
        auto array = [&type]() { if (type.size() < 2 || type.compare(type.size() - 2, 2, "[]") != 0) type += "[]"; };
        if (token.equalsControlSymbol(controlSymbols::openSquare)) {
            Token next = tokenizer.next();
            if (next.type == Type::literal && std::isdigit(static_cast<unsigned char>(next.value[0]))) next = tokenizer.next();
            if (!next.equalsControlSymbol(controlSymbols::closeSquare)) throw syntactical_error("Expected ']', found '" + next.value + "'");
            array();
            return true;
        }
        if (isWord(token, "array")) {
            array();
            return true;
        }
        if (type == "double" && isWord(token, "precision")) return true;
        if (type == "character" && isWord(token, "varying")) {
            type = "varchar";
            return true;
        }
        return false;
    }

    /// Reads KEY after PRIMARY of a column: the column is the key and not null
    static void columnPrimaryKey(Tokenizer & tokenizer, CreateTableStatement & stmt) {
        Token key = tokenizer.next();
        if (!isWord(key, "key")) throw syntactical_error("Expected 'KEY', found '" + key.value + "'");
        if (!stmt.primaryKey.empty()) throw syntactical_error("a table has one primary key");
        stmt.primaryKey.push_back(stmt.columns.back().name);
        stmt.columns.back().nullable = false;
    }

    /// \returns The format of COPY named by \p token: csv or tbl
    static std::string copyFormat(const Token & token) {
        if (isWord(token, "csv") || isWord(token, "tbl")) {
            std::string format = token.value;
            std::transform(format.begin(), format.end(), format.begin(), ::tolower);
            return format;
        }
        if (isWord(token, "text") || isWord(token, "binary")) {
            throw syntactical_error("COPY reads and writes the formats csv and tbl, not " + token.value);
        }
        throw syntactical_error("Expected format name, found '" + token.value + "'");
    }

    /// Sets an option of COPY that takes a string (DELIMITER, NULL, QUOTE, ESCAPE)
    static void setCopyOption(CopyStatement & stmt, const Token & option, const Token & value) {
        if (!value.hasType(Type::literal) || !value.quoted) {
            throw syntactical_error("Expected a string after " + option.value + ", found '" + value.value + "'");
        }
        if (isWord(option, "delimiter")) stmt.delimiter = value.value;
        else if (isWord(option, "null")) stmt.null = value.value;
        else if (isWord(option, "quote")) stmt.quote = value.value;
        else stmt.escape = value.value;
    }

    static bool isCopyOption(const Token & token) {
        return isWord(token, "delimiter") || isWord(token, "null") || isWord(token, "quote") || isWord(token, "escape");
    }

    /// Reads the options of COPY after the file and the end of the statement, as PostgreSQL either
    ///   [ WITH ] ( FORMAT csv|tbl [, HEADER [ boolean ] ] [, DELIMITER 'c' ] [, NULL 'text' ] [, QUOTE 'c' ] [, ESCAPE 'c' ] )
    /// or in the older form
    ///   [ WITH ] [ FORMAT ] csv|tbl [ HEADER ] [ DELIMITER [ AS ] 'c' ] [ NULL [ AS ] 'text' ] [ QUOTE [ AS ] 'c' ] [ ESCAPE [ AS ] 'c' ]
    static void copyOptions(Tokenizer & tokenizer, CopyStatement & stmt, Token token) {
        if (token.equalsKeyword(Keyword::With)) token = tokenizer.next();
        if (token.equalsControlSymbol(controlSymbols::openBracket)) {
            for (;;) {
                const Token option = tokenizer.next();
                Token next = tokenizer.next();
                if (isWord(option, "format")) {
                    stmt.format = copyFormat(next);
                    next = tokenizer.next();
                } else if (isWord(option, "header")) {
                    // HEADER alone is HEADER true
                    stmt.header = true;
                    if (!next.equalsControlSymbol(controlSymbols::separator) && !next.equalsControlSymbol(controlSymbols::closeBracket)) {
                        std::string value = next.value; // a word, a constant or a string
                        std::transform(value.begin(), value.end(), value.begin(), ::tolower);
                        if (value == "false" || value == "off" || value == "0") stmt.header = false;
                        else if (value != "true" && value != "on" && value != "1") {
                            throw syntactical_error("HEADER takes a boolean, found '" + next.value + "'");
                        }
                        next = tokenizer.next();
                    }
                } else if (isCopyOption(option)) {
                    setCopyOption(stmt, option, next);
                    next = tokenizer.next();
                } else {
                    throw syntactical_error("COPY has no option '" + option.value + "'");
                }
                if (next.equalsControlSymbol(controlSymbols::closeBracket)) break;
                if (!next.equalsControlSymbol(controlSymbols::separator)) {
                    throw syntactical_error("Expected ',' or ')', found '" + next.value + "'");
                }
            }
            token = tokenizer.next();
        } else {
            if (isWord(token, "format")) token = tokenizer.next();
            stmt.format = copyFormat(token);
            for (token = tokenizer.next(); !token.hasType(Type::delimiter); token = tokenizer.next()) {
                if (isWord(token, "header")) stmt.header = true;
                else if (isCopyOption(token)) {
                    Token value = tokenizer.next();
                    if (isWord(value, "as")) value = tokenizer.next();
                    setCopyOption(stmt, token, value);
                }
                else throw syntactical_error("COPY has no option '" + token.value + "'");
            }
        }
        if (!token.hasType(Type::delimiter)) throw syntactical_error("Expected ';', found '" + token.value + "'");
    }

    void SQLParser::parseToken(Tokenizer &tokenizer, ParsingContext &context) {
        const Token next = tokenizer.next(); // a copy: later calls of next() may move the tokens
        // NULL where a value is expected is a constant like the others
        const bool valueExpected = context.state == State::InsertValuesBegin || context.state == State::InsertValueSeperator ||
                context.state == State::UpdateSetExprOp || context.state == State::UpdateSetExprArith ||
                context.state == State::AlterTableDefault;
        const Token nullConstant = valueToken(next);
        const Token &token = valueExpected ? nullConstant : next;
        // When the token is a delimiter check if the parser is in a valid final state
        if (token.hasType(Type::delimiter)) {
            if (!context.stateIsFinal()) {
                throw syntactical_error("unexpected end of input");
            }
            context.state = State::Done;
            return;
        }

        // Implements state transitions
        switch (context.state) {
            case State::Init:
                if (token.equalsKeyword(Keyword::Select)) {
                    context.opType = ParsingContext::OpType::Select;
                    context.selectStmt = new SelectStatement();
                    context.state = State::Select;
                } else if (token.equalsKeyword(Keyword::Insert)) {
                    context.opType = ParsingContext::OpType::Insert;
                    context.insertStmt = new InsertStatement();
                    context.state = State::Insert;
                } else if (token.equalsKeyword(Keyword::Update)) {
                    context.opType = ParsingContext::OpType::Update;
                    context.updateStmt = new UpdateStatement();
                    context.state = State::Update;
                } else if (token.equalsKeyword(Keyword::Delete)) {
                    context.opType = ParsingContext::OpType::Delete;
                    context.deleteStmt = new DeleteStatement();
                    context.state = State::Delete;
                } else if (token.equalsKeyword(Keyword::Create)) {
                    context.state = State::Create;
                } else if (token.equalsKeyword(Keyword::Branch)) {
                    context.opType = ParsingContext::OpType::Branch;
                    context.state = State::Branch;
                } else if (token.equalsKeyword(Keyword::Copy)) {
                    context.opType = ParsingContext::OpType::Copy;
                    context.copyStmt = new CopyStatement();
                    context.state = State::Copy;
                } else if (token.equalsKeyword(Keyword::Drop)) {
                    context.state = State::Drop;
                } else if (token.equalsKeyword(Keyword::Merge)) {
                    context.opType = ParsingContext::OpType::MergeBranch;
                    context.mergeBranchStmt = new MergeBranchStatement();
                    context.state = State::Merge;
                } else if (token.equalsKeyword(Keyword::Alter)) {
                    context.state = State::Alter;
                } else {
                    throw syntactical_error("Expected 'Select', 'Insert', 'Update', 'Delete' , 'BRANCH', 'COPY', 'Create', 'DROP', 'MERGE' or 'ALTER', found '" + token.value + "'");
                }
                break;

                //
                //  ALTER TABLE t [ VERSION b ] ADD [ COLUMN ] c type [ NOT NULL ] DEFAULT v
                //  ALTER TABLE t [ VERSION b ] DROP [ COLUMN ] c
                //
            case State::Alter:
                if (token.equalsKeyword(Keyword::Table)) {
                    context.opType = ParsingContext::OpType::AlterTable;
                    context.alterTableStmt = new AlterTableStatement();
                    context.alterTableStmt->relation.version = context.defaultBranch;
                    context.state = State::AlterTable;
                } else {
                    throw syntactical_error("Expected 'TABLE', found '" + token.value + "'");
                }
                break;
            case State::AlterTable:
                if (token.hasType(Type::identifier)) {
                    context.alterTableStmt->relation.name = token.value;
                    context.state = State::AlterTableName;
                } else {
                    throw syntactical_error("Expected table name, found '" + token.value + "'");
                }
                break;
            case State::AlterTableName:
                if (token.equalsKeyword(Keyword::Version)) {
                    context.state = State::AlterTableVersion;
                    break;
                }
                // fall through: VERSION is optional
            case State::AlterTableTag:
                if (token.equalsKeyword(Keyword::Add)) {
                    context.alterTableStmt->add = true;
                    context.state = State::AlterTableAdd;
                } else if (token.equalsKeyword(Keyword::Drop)) {
                    context.alterTableStmt->add = false;
                    context.state = State::AlterTableDrop;
                } else {
                    throw syntactical_error("Expected 'VERSION', 'ADD' or 'DROP', found '" + token.value + "'");
                }
                break;
            case State::AlterTableVersion:
                if (token.hasType(Type::identifier)) {
                    context.alterTableStmt->relation.version = token.value;
                    context.state = State::AlterTableTag;
                } else {
                    throw syntactical_error("Expected version name, found '" + token.value + "'");
                }
                break;
            case State::AlterTableAdd:
                if (token.equalsKeyword(Keyword::Column)) {
                    context.state = State::AlterTableAddColumn;
                    break;
                }
                // fall through: COLUMN is optional
            case State::AlterTableAddColumn:
                if (token.hasType(Type::identifier)) {
                    context.alterTableStmt->column.name = token.value;
                    context.state = State::AlterTableColumnName;
                } else {
                    throw syntactical_error("Expected column name, found '" + token.value + "'");
                }
                break;
            case State::AlterTableColumnName:
                if (token.hasType(Type::identifier)) {
                    auto &column = context.alterTableStmt->column;
                    column.type.clear();
                    std::transform(token.value.begin(), token.value.end(), std::back_inserter(column.type), tolower);
                    column.length = 0;
                    column.precision = 0;
                    column.nullable = true;
                    context.state = State::AlterTableColumnType;
                } else {
                    throw syntactical_error("Expected type name, found '" + token.value + "'");
                }
                break;
            case State::AlterTableColumnType:
                if (token.equalsControlSymbol(controlSymbols::openBracket) &&
                    context.alterTableStmt->column.type.find("[]") == std::string::npos) {
                    context.state = State::AlterTableTypeDetailBegin;
                    break;
                }
                // fall through: the type details are optional
            case State::AlterTableTypeDetailEnd:
                if (typeSuffix(tokenizer, token, context.alterTableStmt->column.type)) {
                    break;
                } else if (token.equalsKeyword(Keyword::Not)) {
                    context.state = State::AlterTableTypeNot;
                } else if (token.equalsKeyword(Keyword::Null)) {
                    context.state = State::AlterTableTypeNotNull; // nullable, as without
                } else if (token.equalsKeyword(Keyword::Default)) {
                    context.state = State::AlterTableDefault;
                } else {
                    throw syntactical_error("Expected 'NOT', 'NULL' or 'DEFAULT', found '" + token.value + "'");
                }
                break;
            case State::AlterTableTypeDetailBegin:
                if (token.hasType(Type::literal) && std::isdigit(token.value[0])) {
                    context.alterTableStmt->column.length = std::stoi(token.value);
                    context.state = State::AlterTableTypeDetailLength;
                } else {
                    throw syntactical_error("Expected type length, found '" + token.value + "'");
                }
                break;
            case State::AlterTableTypeDetailLength:
                if (token.equalsControlSymbol(controlSymbols::closeBracket)) {
                    context.state = State::AlterTableTypeDetailEnd;
                } else if (token.equalsControlSymbol(controlSymbols::separator)) {
                    context.state = State::AlterTableTypeDetailSeparator;
                } else {
                    throw syntactical_error("Expected ',' or ')', found '" + token.value + "'");
                }
                break;
            case State::AlterTableTypeDetailSeparator:
                if (token.hasType(Type::literal) && std::isdigit(token.value[0])) {
                    context.alterTableStmt->column.precision = std::stoi(token.value);
                    context.state = State::AlterTableTypeDetailPrecision;
                } else {
                    throw syntactical_error("Expected type precision, found '" + token.value + "'");
                }
                break;
            case State::AlterTableTypeDetailPrecision:
                if (token.equalsControlSymbol(controlSymbols::closeBracket)) {
                    context.state = State::AlterTableTypeDetailEnd;
                } else {
                    throw syntactical_error("Expected ')', found '" + token.value + "'");
                }
                break;
            case State::AlterTableTypeNot:
                if (token.equalsKeyword(Keyword::Null)) {
                    context.alterTableStmt->column.nullable = false;
                    context.state = State::AlterTableTypeNotNull;
                } else {
                    throw syntactical_error("Expected 'NULL', found '" + token.value + "'");
                }
                break;
            case State::AlterTableTypeNotNull:
                if (token.equalsKeyword(Keyword::Default)) {
                    context.state = State::AlterTableDefault;
                } else {
                    throw syntactical_error("Expected 'DEFAULT', found '" + token.value + "'");
                }
                break;
            case State::AlterTableDefault:
                if (token.hasType(Type::literal) || token.hasType(Type::identifier)) {
                    context.alterTableStmt->defaultValue = token.value;
                    context.alterTableStmt->hasDefault = true;
                    context.state = State::AlterTableDefaultValue;
                } else {
                    throw syntactical_error("Expected default value, found '" + token.value + "'");
                }
                break;
            case State::AlterTableDrop:
                if (token.equalsKeyword(Keyword::Column)) {
                    context.state = State::AlterTableDropColumn;
                    break;
                }
                // fall through: COLUMN is optional
            case State::AlterTableDropColumn:
                if (token.hasType(Type::identifier)) {
                    context.alterTableStmt->column.name = token.value;
                    context.state = State::AlterTableDropName;
                } else {
                    throw syntactical_error("Expected column name, found '" + token.value + "'");
                }
                break;

                //
                //  DROP BRANCH b [ CASCADE ]
                //
            case State::Drop:
                if (token.equalsKeyword(Keyword::Branch)) {
                    context.opType = ParsingContext::OpType::DropBranch;
                    context.dropBranchStmt = new DropBranchStatement();
                    context.state = State::DropBranch;
                } else {
                    throw syntactical_error("Expected 'BRANCH', found '" + token.value + "'");
                }
                break;
            case State::DropBranch:
                if (token.hasType(Type::identifier)) {
                    context.dropBranchStmt->branchName = token.value;
                    context.state = State::DropBranchName;
                } else {
                    throw syntactical_error("Expected branch name, found '" + token.value + "'");
                }
                break;
            case State::DropBranchName:
                if (token.equalsKeyword(Keyword::Cascade)) {
                    context.dropBranchStmt->cascade = true;
                    context.state = State::DropBranchCascade;
                } else {
                    throw syntactical_error("Expected 'CASCADE', found '" + token.value + "'");
                }
                break;

                //
                //  MERGE [ BRANCH ] b INTO p
                //
            case State::Merge:
                if (token.equalsKeyword(Keyword::Branch)) {
                    context.state = State::MergeBranch;
                    break;
                }
                // fall through: BRANCH is optional
            case State::MergeBranch:
                if (token.hasType(Type::identifier)) {
                    context.mergeBranchStmt->branchName = token.value;
                    context.state = State::MergeBranchName;
                } else {
                    throw syntactical_error("Expected branch name, found '" + token.value + "'");
                }
                break;
            case State::MergeBranchName:
                if (token.equalsKeyword(Keyword::Into)) {
                    context.state = State::MergeBranchInto;
                } else {
                    throw syntactical_error("Expected 'INTO', found '" + token.value + "'");
                }
                break;
            case State::MergeBranchInto:
                if (token.hasType(Type::identifier)) {
                    context.mergeBranchStmt->targetBranchName = token.value;
                    context.state = State::MergeBranchTarget;
                } else {
                    throw syntactical_error("Expected branch name, found '" + token.value + "'");
                }
                break;
            case State::MergeBranchTarget: {
                // ON CONFLICT KEEP TARGET | KEEP SOURCE
                Token conflict = tokenizer.next();
                Token keep = tokenizer.next();
                Token side = tokenizer.next();
                if (!token.equalsKeyword(Keyword::On) || !isWord(conflict, "conflict") || !isWord(keep, "keep") ||
                    !(isWord(side, "target") || isWord(side, "source")) || !context.mergeBranchStmt->onConflict.empty()) {
                    throw syntactical_error("Expected ON CONFLICT KEEP TARGET or KEEP SOURCE after the target branch");
                }
                context.mergeBranchStmt->onConflict = isWord(side, "target") ? "target" : "source";
                break;
            }

                //
                //  Copy
                //
            case State::Copy:
                if (token.hasType(Type::identifier)) {
                    context.copyStmt->relation.name = token.value;
                    context.state = State::CopyTable;
                } else {
                    throw syntactical_error("Expected table name, found '" + token.value + "'");
                }
                break;
            case State::CopyTable:
                if (token.equalsKeyword(Keyword::From)) {
                    context.copyStmt->directionFrom = true;
                    context.copyStmt->relation.version = context.defaultBranch;
                    context.state = State::CopyDirection;
                } else if (token.equalsKeyword(Keyword::To)) {
                    context.copyStmt->directionFrom = false;
                    context.copyStmt->relation.version = context.defaultBranch;
                    context.state = State::CopyDirection;
                } else if (token.equalsKeyword(Keyword::Version)) {
                    context.state = State::CopyVersion;
                } else {
                    throw syntactical_error("Expected 'FROM','VERSION' OR 'TO', found '" + token.value + "'");
                }
                break;
            case State::CopyVersion:
                if (token.hasType(Type::identifier)) {
                    context.copyStmt->relation.version = token.value;
                    context.state = State::CopyTag;
                } else {
                    throw syntactical_error("Expected branch name, found '" + token.value + "'");
                }
                break;
            case State::CopyTag:
                if (token.equalsKeyword(Keyword::To) || token.equalsKeyword(Keyword::From)) {
                    context.copyStmt->directionFrom = token.equalsKeyword(Keyword::From);
                    context.state = State::CopyDirection;
                } else {
                    throw syntactical_error("Expected 'FROM' or 'TO', found '" + token.value + "'");
                }
                break;
            case State::CopyDirection:
                if (token.hasType(Type::literal) && token.quoted) {
                    context.copyStmt->filePath = token.value;
                    context.state = State::CopyPath;
                } else {
                    throw syntactical_error("Expected file path, found '" + token.value + "'");
                }
                break;
            case State::CopyPath:
                copyOptions(tokenizer, *context.copyStmt, token);
                context.state = State::Done;
                break;

                //
                //  CREATE
                //
            case State::Create:
                if (token.equalsKeyword(Keyword::Table)) {
                    context.opType = ParsingContext::OpType::CreateTable;
                    context.createTableStmt = new CreateTableStatement();
                    context.state = State::CreateTable;
                } else if (token.equalsKeyword(Keyword::Branch)) {
                    context.opType = ParsingContext::OpType::CreateBranch;
                    context.createBranchStmt = new CreateBranchStatement();
                    context.state = State::CreateBranch;
                } else if (token.equalsKeyword(Keyword::Index)) {
                    context.opType = ParsingContext::OpType::CreateIndex;
                    context.createIndexStmt = new CreateIndexStatement();
                    context.createIndexStmt->relation.version = context.defaultBranch;
                    context.state = State::CreateIndex;
                } else {
                    throw syntactical_error("Expected 'TABLE', 'BRANCH' or 'INDEX', found '" + token.value + "'");
                }
                break;

                //
                //  CREATE INDEX [ name ] ON t ( c ) [ VERSION b ]
                //
            case State::CreateIndex:
                if (token.hasType(Type::identifier)) {
                    context.createIndexStmt->indexName = token.value;
                    context.state = State::CreateIndexName;
                    break;
                }
                // fall through: the name is optional
            case State::CreateIndexName:
                if (token.equalsKeyword(Keyword::On)) {
                    context.state = State::CreateIndexOn;
                } else {
                    throw syntactical_error("Expected index name or 'ON', found '" + token.value + "'");
                }
                break;
            case State::CreateIndexOn:
                if (token.hasType(Type::identifier)) {
                    context.createIndexStmt->relation.name = token.value;
                    context.state = State::CreateIndexTable;
                } else {
                    throw syntactical_error("Expected table name, found '" + token.value + "'");
                }
                break;
            case State::CreateIndexTable:
                if (token.equalsControlSymbol(controlSymbols::openBracket)) {
                    context.state = State::CreateIndexColumnsBegin;
                } else {
                    throw syntactical_error("Expected '(', found '" + token.value + "'");
                }
                break;
            case State::CreateIndexColumnsBegin:
            case State::CreateIndexColumnSeparator:
                if (token.hasType(Type::identifier)) {
                    context.createIndexStmt->columns.push_back(Column());
                    context.createIndexStmt->columns.back().name = token.value;
                    context.state = State::CreateIndexColumnName;
                } else {
                    throw syntactical_error("Expected column name, found '" + token.value + "'");
                }
                break;
            case State::CreateIndexColumnName:
                if (token.equalsControlSymbol(controlSymbols::separator)) {
                    context.state = State::CreateIndexColumnSeparator;
                } else if (token.equalsControlSymbol(controlSymbols::closeBracket)) {
                    context.state = State::CreateIndexColumnsEnd;
                } else {
                    throw syntactical_error("Expected ',' or ')', found '" + token.value + "'");
                }
                break;
            case State::CreateIndexColumnsEnd:
                if (token.equalsKeyword(Keyword::Version)) {
                    context.state = State::CreateIndexVersion;
                } else {
                    throw syntactical_error("Expected 'VERSION', found '" + token.value + "'");
                }
                break;
            case State::CreateIndexVersion:
                if (token.hasType(Type::identifier)) {
                    context.createIndexStmt->relation.version = token.value;
                    context.state = State::CreateIndexTag;
                } else {
                    throw syntactical_error("Expected version name, found '" + token.value + "'");
                }
                break;
            case State::CreateBranch:
                if (token.type == Type::identifier) {
                    context.createBranchStmt->branchName = token.value;
                    context.state = State::CreateBranchTag;
                } else {
                    throw syntactical_error("Expected branch name, found '" + token.value + "'");
                }
                break;
            case State::CreateBranchTag:
                if (token.equalsKeyword(Keyword::From)) {
                    context.state = State::CreateBranchFrom;
                } else {
                    throw syntactical_error("Expected 'FROM', found '" + token.value + "'");
                }
                break;
            case State::CreateBranchFrom:
                if (token.type == Type::identifier) {
                    context.createBranchStmt->parentBranchName = token.value;
                    context.state = State::CreateBranchParent;
                } else {
                    throw syntactical_error("Expected parent branch name, found '" + token.value + "'");
                }
                break;

            case State::CreateTable:
                if (token.type == Type::identifier) {
                    context.createTableStmt->tableName = token.value;
                    context.state = State::CreateTableRelationName;
                } else {
                    throw syntactical_error("Expected table name, found '" + token.value + "'");
                }
                break;
            case State::CreateTableRelationName:
                if (token.equalsControlSymbol(controlSymbols::openBracket)) {
                    context.state = State::CreateTableColumnsBegin;
                }
                break;
            case State::CreateTableColumnsBegin:
            case State::CreateTableColumnSeperator:
                if (isWord(token, "primary")) {
                    auto & stmt = *context.createTableStmt;
                    Token key = tokenizer.next();
                    Token open = tokenizer.next();
                    if (!isWord(key, "key") || !open.equalsControlSymbol(controlSymbols::openBracket)) {
                        throw syntactical_error("Expected 'KEY (' after 'PRIMARY'");
                    }
                    if (!stmt.primaryKey.empty()) throw syntactical_error("a table has one primary key");
                    for (auto & column : readList(tokenizer)) {
                        if (column.type != Type::identifier) throw syntactical_error("Expected column name, found '" + column.value + "'");
                        stmt.primaryKey.push_back(column.value);
                    }
                    Token next = tokenizer.next();
                    if (next.equalsControlSymbol(controlSymbols::closeBracket)) {
                        context.state = CreateTableColumnsEnd;
                    } else if (next.equalsControlSymbol(controlSymbols::separator)) {
                        context.state = CreateTableColumnSeperator;
                    } else {
                        throw syntactical_error("Expected ',' or ')', found '" + next.value + "'");
                    }
                } else if (token.type == Type::identifier) {
                    context.createTableStmt->columns.push_back(ColumnSpec());
                    context.createTableStmt->columns.back().name = token.value;
                    context.state = CreateTableColumnName;
                } else {
                    throw syntactical_error("Expected column name, found '" + token.value + "'");
                }
                break;
            case State::CreateTableColumnName:
                if (token.type == Type::identifier) {
                    std::string lowercase_token_value;
                    std::transform(token.value.begin(), token.value.end(), std::back_inserter(lowercase_token_value), tolower);
                    context.createTableStmt->columns.back().type = lowercase_token_value;
                    context.state = CreateTableColumnType;
                } else {
                    throw syntactical_error("Expected column name, found '" + token.value + "'");
                }
                break;
            case State::CreateTableColumnType:
                if (token.equalsControlSymbol(controlSymbols::openBracket) &&
                    context.createTableStmt->columns.back().type.find("[]") == std::string::npos) {
                    context.state = CreateTableTypeDetailBegin;
                    break;
                }
                if (typeSuffix(tokenizer, token, context.createTableStmt->columns.back().type)) {
                    break;
                }
                context.createTableStmt->columns.back().length = 0;
                context.createTableStmt->columns.back().precision = 0;
                if (token.equalsKeyword(Keyword::Not)) {
                    context.state = CreateTableTypeNot;
                    break;
                }
                if (token.equalsKeyword(Keyword::Null)) {
                    context.createTableStmt->columns.back().nullable = true;
                    context.state = State::CreateTableTypeNotNull;
                    break;
                }
                if (isWord(token, "primary")) {
                    columnPrimaryKey(tokenizer, *context.createTableStmt);
                    context.state = State::CreateTableTypeNotNull;
                    break;
                }
                context.createTableStmt->columns.back().nullable = true;

                if (token.equalsControlSymbol(controlSymbols::closeBracket)) {
                    context.state = CreateTableColumnsEnd;
                } else if (token.equalsControlSymbol(controlSymbols::separator)) {
                    context.state = CreateTableColumnSeperator;
                } else {
                    throw syntactical_error("Expected '(' , ',' , ')' or 'NOT', found '" + token.value + "'");
                }
                break;
            case State::CreateTableTypeDetailBegin:
                if (token.type == literal && std::isdigit(token.value[0])) {
                    context.createTableStmt->columns.back().length = std::stoi(token.value);
                    context.state = CreateTableTypeDetailLength;
                } else {
                    throw syntactical_error("Expected type length, found '" + token.value + "'");
                }
                break;
            case State::CreateTableTypeDetailLength:
                if (token.equalsControlSymbol(controlSymbols::closeBracket)) {
                    context.createTableStmt->columns.back().precision = 0;
                    context.state = CreateTableTypeDetailEnd;
                } else if (token.equalsControlSymbol(controlSymbols::separator)) {
                    context.state = CreateTableTypeDetailSeperator;
                } else {
                    throw syntactical_error("Expected ',' or ')', found '" + token.value + "'");
                }
                break;
            case State::CreateTableTypeDetailSeperator:
                if (token.type == literal && std::isdigit(token.value[0])) {
                    context.createTableStmt->columns.back().precision = std::stoi(token.value);
                    context.state = CreateTableTypeDetailPrecision;
                } else {
                    throw syntactical_error("Expected type precision, found '" + token.value + "'");
                }
                break;
            case State::CreateTableTypeDetailPrecision:
                if (token.equalsControlSymbol(controlSymbols::closeBracket)) {
                    context.state = CreateTableTypeDetailEnd;
                } else {
                    throw syntactical_error("Expected ')', found '" + token.value + "'");
                }
                break;
            case State::CreateTableTypeDetailEnd:
                if (typeSuffix(tokenizer, token, context.createTableStmt->columns.back().type)) {
                    break;
                }
                if (token.equalsKeyword(Keyword::Not)) {
                    context.state = CreateTableTypeNot;
                    break;
                }
                if (token.equalsKeyword(Keyword::Null)) {
                    context.createTableStmt->columns.back().nullable = true;
                    context.state = State::CreateTableTypeNotNull;
                    break;
                }
                if (isWord(token, "primary")) {
                    columnPrimaryKey(tokenizer, *context.createTableStmt);
                    context.state = State::CreateTableTypeNotNull;
                    break;
                }
                if (token.equalsControlSymbol(controlSymbols::closeBracket)) {
                    context.state = CreateTableColumnsEnd;
                } else if (token.equalsControlSymbol(controlSymbols::separator)) {
                    context.state = CreateTableColumnSeperator;
                } else {
                    throw syntactical_error("Expected ',' , ')' or 'NOT', found '" + token.value + "'");
                }
                context.createTableStmt->columns.back().nullable = true;
                break;
            case State::CreateTableTypeNot:
                if (token.equalsKeyword(Keyword::Null)) {
                    context.createTableStmt->columns.back().nullable = false;
                    context.state = State::CreateTableTypeNotNull;
                } else {
                    throw syntactical_error("Expected 'NULL', found '" + token.value + "'");
                }
                break;
            case State::CreateTableTypeNotNull:
                if (isWord(token, "primary")) {
                    columnPrimaryKey(tokenizer, *context.createTableStmt);
                    break;
                }
                if (token.equalsControlSymbol(controlSymbols::closeBracket)) {
                    context.state = CreateTableColumnsEnd;
                } else if (token.equalsControlSymbol(controlSymbols::separator)) {
                    context.state = CreateTableColumnSeperator;
                } else {
                    throw syntactical_error("Expected ',' , ')', found '" + token.value + "'");
                }
                break;

                //
                //  DELETE
                //
            case State::Delete:
                if (token.equalsKeyword(Keyword::From)) {
                    context.state = State::DeleteFrom;
                } else {
                    throw syntactical_error("Expected 'WHERE', found '" + token.value + "'");
                }
                break;
            case State::DeleteFrom:
                if (token.type == Type::identifier) {
                    context.deleteStmt->relation.name = token.value;
                    context.deleteStmt->relation.version = context.defaultBranch;
                    context.state = DeleteRelationName;
                } else {
                    throw syntactical_error("Expected table name, found '" + token.value + "'");
                }
                break;
            case State::DeleteRelationName:
                if (token.equalsKeyword(Keyword::Version)) {
                    context.state = State::DeleteVersion;
                } else if (token.equalsKeyword(Keyword::Where)) {
                    context.state = State::DeleteWhere;
                } else {
                    throw syntactical_error("Expected 'VERSION' or 'WHERE', found '" + token.value + "'");
                }
                break;
            case State::DeleteVersion:
                if (token.type == Type::identifier) {
                    context.deleteStmt->relation.version = token.value;
                    context.state = DeleteTag;
                } else {
                    throw syntactical_error("Expected version name, found '" + token.value + "'");
                }
                break;
            case State::DeleteTag:
                if (token.equalsKeyword(Keyword::Where)) {
                    context.state = State::DeleteWhere;
                } else {
                    throw syntactical_error("Expected 'WHERE', found '" + token.value + "'");
                }
                break;
            case State::DeleteWhere:
                parseWhere(tokenizer, token, context.deleteStmt->selections, context.deleteStmt->predicates, nullptr, false);
                context.state = State::Done;
                break;
            case State::DeleteWhereExprLhs:
                if (token.type == Type::op) {
                    context.state = State::DeleteWhereExprOp;
                } else {
                    throw syntactical_error("Expected '=', found '" + token.value + "'");
                }
                break;
            case State::DeleteWhereExprOp:
                if (token.type == Type::literal) {
                    context.deleteStmt->selections.push_back(std::make_pair(Column(),token.value));
                    context.deleteStmt->selections.back().first.name = tokenizer.prev(2).value;
                    context.state = State::DeleteWhereExprRhs;
                } else {
                    throw syntactical_error("Expected right expression, found '" + token.value + "'");
                }
                break;
            case State::DeleteWhereExprRhs:
                if (token.equalsKeyword(Keyword::And)) {
                    context.state = State::DeleteWhereAnd;
                } else {
                    throw syntactical_error("Expected 'AND', found '" + token.value + "'");
                }
                break;

                //
                //  UPDATE
                //
            case State::Update:
                if (token.type == Type::identifier) {
                    context.updateStmt->relation.name = token.value;
                    context.state = UpdateRelationName;
                } else {
                    throw syntactical_error("Expected table name, found '" + token.value + "'");
                }
                break;
            case State::UpdateRelationName:
                if (token.equalsKeyword(Keyword::Version)) {
                    context.state = State::UpdateVersion;
                } else if (token.equalsKeyword(Keyword::Set)) {
                    context.updateStmt->relation.version = context.defaultBranch;
                    context.state = State::UpdateSet;
                } else {
                    throw syntactical_error("Expected 'VERSION', 'SET', found '" + token.value + "'");
                }
                break;
            case State::UpdateVersion:
                if (token.hasType(Type::identifier)) {
                    context.updateStmt->relation.version = token.value;
                    context.state = UpdateTag;
                } else {
                    throw syntactical_error("Expected version name, found '" + token.value + "'");
                }
                break;
            case State::UpdateTag:
                if (token.equalsKeyword(Keyword::Set)) {
                    context.state = State::UpdateSet;
                } else {
                    throw syntactical_error("Expected 'SET', found '" + token.value + "'");
                }
                break;
            case State::UpdateSet:
            case State::UpdateSetSeperator:
                if (token.type == identifier) {
                    context.state = State::UpdateSetExprLhs;
                } else {
                    throw syntactical_error("Expected left expression, found '" + token.value + "'");
                }
                break;
            case State::UpdateSetExprLhs:
                if (token.type == Type::op) {
                    context.state = State::UpdateSetExprOp;
                } else {
                    throw syntactical_error("Expected '=', found '" + token.value + "'");
                }
                break;
            case State::UpdateSetExprOp: {
                // the terms of + - * / of constants and columns, or any expression
                const std::string target = tokenizer.prev(2).value;
                ExpressionParser parser(tokenizer, token);
                Node e = parser.parse();
                auto & stmt = *context.updateStmt;
                std::vector<UpdateTerm> terms;
                const bool simple = toTerms(e, terms);
                stmt.updates.push_back(std::make_pair(Column(), simple && terms.size() == 1 && terms[0].column.name.empty() ? terms[0].value : ""));
                stmt.updates.back().first.name = target;
                stmt.expressions.push_back(simple ? terms : std::vector<UpdateTerm>());
                stmt.trees.push_back(simple ? nullptr : std::make_shared<Node>(std::move(e)));
                const Token & end = parser.token();
                if (end.equalsControlSymbol(controlSymbols::separator)) {
                    context.state = State::UpdateSetSeperator;
                } else if (end.equalsKeyword(Keyword::Where)) {
                    context.state = State::UpdateWhere;
                } else if (end.hasType(Type::delimiter)) {
                    context.state = State::Done;
                } else {
                    throw syntactical_error("Expected ',' or 'WHERE', found '" + end.value + "'");
                }
                break;
            }
            case State::UpdateSetExprArith:
                if (token.type == Type::literal || token.type == Type::identifier) {
                    UpdateTerm term;
                    if (context.state == State::UpdateSetExprArith) {
                        term.op = tokenizer.prev(1).value;
                    } else {
                        // the first term; a constant alone is also kept as the value of the update
                        context.updateStmt->updates.push_back(std::make_pair(Column(), token.type == Type::literal ? token.value : ""));
                        context.updateStmt->updates.back().first.name = tokenizer.prev(2).value;
                        context.updateStmt->expressions.emplace_back();
                    }
                    if (token.type == Type::identifier) {
                        BindingAttribute columnBinding = parse_binding_attribute(token.value);
                        term.column.name = columnBinding.second;
                        term.column.table = columnBinding.first;
                    } else {
                        term.value = token.value;
                    }
                    context.updateStmt->expressions.back().push_back(term);
                    context.state = State::UpdateSetExprRhs;
                } else {
                    throw syntactical_error("Expected right expression, found '" + token.value + "'");
                }
                break;
            case State::UpdateSetExprRhs:
                if (isArithmeticOperator(token)) {
                    context.state = State::UpdateSetExprArith;
                } else if (token.equalsControlSymbol(controlSymbols::separator)) {
                    context.state = State::UpdateSetSeperator;
                } else if (token.equalsKeyword(Keyword::Where)) {
                    context.state = State::UpdateWhere;
                } else {
                    throw syntactical_error("Expected '+', '-', '*', '/', ',' or 'WHERE', found '" + token.value + "'");
                }
                break;
            case State::UpdateWhere:
                parseWhere(tokenizer, token, context.updateStmt->selections, context.updateStmt->predicates, nullptr, false);
                context.state = State::Done;
                break;
            case State::UpdateWhereExprLhs:
                if (token.type == Type::op) {
                    context.state = State::UpdateWhereExprOp;
                } else {
                    throw syntactical_error("Expected '=', found '" + token.value + "'");
                }
                break;
            case State::UpdateWhereExprOp:
                if (token.type == Type::literal) {
                    context.updateStmt->selections.push_back(std::make_pair(Column(),token.value));
                    context.updateStmt->selections.back().first.name = tokenizer.prev(2).value;
                    context.state = State::UpdateWhereExprRhs;
                } else {
                    throw syntactical_error("Expected right expression, found '" + token.value + "'");
                }
                break;
            case State::UpdateWhereExprRhs:
                if (token.equalsKeyword(Keyword::And)) {
                    context.state = State::UpdateWhereAnd;
                } else {
                    throw syntactical_error("Expected 'AND', found '" + token.value + "'");
                }
                break;

                //
                //  INSERT
                //
            case State::Insert:
                if (token.equalsKeyword(Keyword::Into)) {
                    context.state = State::InsertInto;
                } else {
                    throw syntactical_error("Expected 'INTO', found '" + token.value + "'");
                }
                break;
            case State::InsertInto:
                if (token.type == Type::identifier) {
                    context.insertStmt->relation.name = token.value;
                    context.state = InsertRelationName;
                } else {
                    throw syntactical_error("Expected table name, found '" + token.value + "'");
                }
                break;
            case State::InsertRelationName:
                if (token.equalsKeyword(Keyword::Version)) {
                    context.state = InsertVersion;
                    break;
                }
                if (token.equalsControlSymbol(controlSymbols::openBracket)) {
                    context.state = InsertColumnsBegin;
                } else if (token.equalsKeyword(Keyword::Values)) {
                    context.state = InsertValues;
                } else {
                    throw syntactical_error("Expected '(', 'VERSION' or 'VALUES', found '" + token.value + "'");
                }
                context.insertStmt->relation.version = context.defaultBranch;
                break;
            case State::InsertVersion:
                if (token.type == Type::identifier) {
                    context.insertStmt->relation.version = token.value;
                    context.state = InsertTag;
                } else {
                    throw syntactical_error("Expected version name, found '" + token.value + "'");
                }
                break;
            case State::InsertTag:
                if (token.equalsControlSymbol(controlSymbols::openBracket)) {
                    context.state = State::InsertColumnsBegin;
                } else if (token.equalsKeyword(Keyword::Values)) {
                    context.state = State::InsertValues;
                } else {
                    throw syntactical_error("Expected '(' or 'VALUES', found '" + token.value + "'");
                }
                break;
            case State::InsertColumnsBegin:
            case State::InsertColumnSeperator:
                if (token.type == Type::identifier) {
                    context.insertStmt->columns.push_back(Column());
                    context.insertStmt->columns.back().name = token.value;
                    context.state = InsertColumnName;
                } else {
                    throw syntactical_error("Expected column name, found '" + token.value + "'");
                }
                break;
            case State::InsertColumnName:
                if (token.equalsControlSymbol(controlSymbols::closeBracket)) {
                    context.state = InsertColumnsEnd;
                } else if (token.equalsControlSymbol(controlSymbols::separator)) {
                    context.state = InsertColumnSeperator;
                } else {
                    throw syntactical_error("Expected ',' , ')', found '" + token.value + "'");
                }
                break;
            case State::InsertColumnsEnd:
                if (token.equalsKeyword(Keyword::Values)) {
                    context.state = InsertValues;
                } else {
                    throw syntactical_error("Expected 'VALUES', found '" + token.value + "'");
                }
                break;
            case State::InsertValues:
                if (token.equalsControlSymbol(controlSymbols::openBracket)) {
                    context.state = InsertValuesBegin;
                } else {
                    throw syntactical_error("Expected '(', found '" + token.value + "'");
                }
                break;
            case State::InsertValuesBegin:
            case State::InsertValueSeperator:
                if (isWord(token, "array")) {
                    // ARRAY[ ... ] of constants, as the text of the array
                    ExpressionParser parser(tokenizer, token);
                    Node array = parser.parse();
                    if (array.kind != Node::Kind::Array) throw syntactical_error("Expected constant, found '" + token.value + "'");
                    auto & stmt = *context.insertStmt;
                    (stmt.rows.empty() ? stmt.values : stmt.rows.back()).push_back(arrayLiteral(array));
                    const Token & end = parser.token();
                    if (end.equalsControlSymbol(controlSymbols::closeBracket)) {
                        context.state = InsertValuesEnd;
                    } else if (end.equalsControlSymbol(controlSymbols::separator)) {
                        context.state = InsertValueSeperator;
                    } else {
                        throw syntactical_error("Expected ',' , ')', found '" + end.value + "'");
                    }
                } else if (token.type == Type::literal) {
                    auto & stmt = *context.insertStmt;
                    (stmt.rows.empty() ? stmt.values : stmt.rows.back()).push_back(token.value);
                    context.state = InsertValue;
                } else {
                    throw syntactical_error("Expected constant, found '" + token.value + "'");
                }
                break;
            case State::InsertValue:
                if (token.equalsControlSymbol(controlSymbols::closeBracket)) {
                    context.state = InsertValuesEnd;
                } else if (token.equalsControlSymbol(controlSymbols::separator)) {
                    context.state = InsertValueSeperator;
                } else {
                    throw syntactical_error("Expected ',' , ')', found '" + token.value + "'");
                }
                break;
            case State::InsertValuesEnd:
                // VALUES ( ... ) , ( ... ): the next row
                if (token.equalsControlSymbol(controlSymbols::separator)) {
                    context.insertStmt->rows.emplace_back();
                    context.state = InsertValues;
                } else {
                    throw syntactical_error("Expected ',' or the end, found '" + token.value + "'");
                }
                break;

                //
                //  SELECT
                //
            case State::Select:
            case State::SelectProjectionAttrSeparator:
                if (token.equalsControlSymbol(controlSymbols::star)) {
                    context.state = State::SelectProjectionStar;
                } else if (token.equalsKeyword(Keyword::Branch)) {
                    // the branch in which a tuple is visible
                    context.selectStmt->projections.push_back(Column());
                    context.selectStmt->projections.back().name = Keyword::Branch;
                    context.state = SelectProjectionAttrName;
                } else {
                    // a column, an aggregate of a column (or count(*)) or any expression, with an alias
                    ExpressionParser parser(tokenizer, token);
                    Node e = parser.parse();
                    Column column;
                    static const std::set<std::string> aggregates = {"count", "sum", "min", "max", "avg"};
                    if (e.kind == Node::Kind::Column) {
                        column.name = e.name;
                        column.table = e.table;
                    } else if (e.kind == Node::Kind::Function && aggregates.count(e.name) &&
                               ((e.name == "count" && e.value == "*") || (e.children.size() == 1 && e.children[0].kind == Node::Kind::Column))) {
                        column.aggregate = e.name;
                        column.name = e.value == "*" ? "*" : e.children[0].name;
                        column.table = e.value == "*" ? "" : e.children[0].table;
                    } else {
                        column.name = outputName(e);
                        column.expression = std::make_shared<Node>(std::move(e));
                    }
                    Token end = parser.token();
                    if (isWord(end, "as")) {
                        end = tokenizer.next();
                        if (end.type != Type::identifier) throw syntactical_error("Expected an alias, found '" + end.value + "'");
                        column.alias = end.value;
                        end = tokenizer.next();
                    } else if (end.type == Type::identifier) {
                        column.alias = end.value;
                        end = tokenizer.next();
                    }
                    context.selectStmt->projections.push_back(std::move(column));
                    if (end.equalsKeyword(Keyword::From)) {
                        context.state = State::SelectFrom;
                    } else if (end.equalsControlSymbol(controlSymbols::separator)) {
                        context.state = State::SelectProjectionAttrSeparator;
                    } else if (end.hasType(Type::delimiter)) {
                        // SELECT without FROM: the expressions of one row
                        context.selectStmt->relations.push_back(Table());
                        context.selectStmt->relations.back().name = sqlExpressions::oneRowTable;
                        context.selectStmt->relations.back().version = "master";
                        context.state = State::Done;
                    } else {
                        throw syntactical_error("Expected ',' or 'FROM', found '" + end.value + "'");
                    }
                }
                break;
            case SelectProjectionStar:
                if (token.equalsKeyword(Keyword::From)) {
                    context.state = State::SelectFrom;
                } else {
                    throw syntactical_error("Expected 'FROM' , found '" + token.value + "'");
                }
                break;
            case State::SelectProjectionAttrName:
                if (token.equalsKeyword(Keyword::From)) {
                    context.state = State::SelectFrom;
                } else if (token.equalsControlSymbol(controlSymbols::separator)) {
                    context.state = State::SelectProjectionAttrSeparator;
                } else if (token.equalsControlSymbol(controlSymbols::openBracket) &&
                           context.selectStmt->projections.back().aggregate.empty() &&
                           context.selectStmt->projections.back().table.empty()) {
                    // the identifier was the name of an aggregate function
                    Column &column = context.selectStmt->projections.back();
                    std::string function;
                    std::transform(column.name.begin(), column.name.end(), std::back_inserter(function), ::tolower);
                    if (function != "count" && function != "sum" && function != "min" && function != "max" && function != "avg") {
                        throw syntactical_error("Unknown aggregate function '" + column.name + "'");
                    }
                    column.aggregate = function;
                    column.name = "";
                    context.state = State::SelectProjectionAggBegin;
                } else {
                    throw syntactical_error("Expected ',' or 'FROM', found '" + token.value + "'");
                }
                break;
            case State::SelectProjectionAggBegin:
                if (token.equalsControlSymbol(controlSymbols::star) && context.selectStmt->projections.back().aggregate == "count") {
                    context.selectStmt->projections.back().name = "*";
                    context.state = State::SelectProjectionAggArg;
                } else if (token.type == Type::identifier) {
                    BindingAttribute columnBinding = parse_binding_attribute(token.value);
                    context.selectStmt->projections.back().name = columnBinding.second;
                    context.selectStmt->projections.back().table = columnBinding.first;
                    context.state = State::SelectProjectionAggArg;
                } else {
                    throw syntactical_error("Expected column name, found '" + token.value + "'");
                }
                break;
            case State::SelectProjectionAggArg:
                if (token.equalsControlSymbol(controlSymbols::closeBracket)) {
                    context.state = State::SelectProjectionAttrName;
                } else {
                    throw syntactical_error("Expected ')', found '" + token.value + "'");
                }
                break;
            case SelectFrom:
            case SelectFromSeparator:
                if (token.type == Type::identifier) {
                    context.selectStmt->relations.push_back(Table());
                    context.selectStmt->relations.back().name = token.value;
                    context.selectStmt->relations.back().alias = "";
                    context.selectStmt->relations.back().version = context.defaultBranch;
                    context.state = SelectFromRelationName;
                } else {
                    throw syntactical_error("Expected table name, found '" + token.value + "'");
                }
                break;
            case State::SelectFromRelationName:
                if (token.equalsControlSymbol(controlSymbols::openBracket) &&
                    equalsIgnoreCase(context.selectStmt->relations.back().name, "diff")) {
                    // DIFF ( t VERSION a , t VERSION b )
                    context.selectStmt->relations.back().name = "";
                    context.selectStmt->relations.back().versionScope = "diff";
                    context.state = SelectFromDiffBegin;
                } else if (token.equalsKeyword(Keyword::Version)) {
                    context.state = SelectFromVersion;
                } else if (token.type == Type::identifier) {
                    context.selectStmt->relations.back().alias = token.value;
                    context.selectStmt->relations.back().version = context.defaultBranch;

                    context.state = State::SelectFromBindingName;
                } else if (token.equalsKeyword(Keyword::Where)) {
                    context.state = State::SelectWhere;
                } else if (token.equalsKeyword(Keyword::Group)) {
                    context.state = State::SelectGroup;
                } else if (token.equalsKeyword(Keyword::Having)) {
                    context.state = State::SelectHaving;
                } else if (token.equalsControlSymbol(controlSymbols::separator)) {
                    context.state = State::SelectFromSeparator;
                } else {
                    throw syntactical_error("Expected binding name, 'VERSION', 'WHERE' or ',' found '" + token.value + "'");
                }
                break;
            case State::SelectFromVersion:
                if (token.type == Type::identifier) {
                    context.selectStmt->relations.back().version = token.value;
                    context.state = SelectFromTag;
                } else if (token.equalsControlSymbol(controlSymbols::openBracket)) {
                    // VERSION ( b1 , b2 , ... )
                    context.selectStmt->relations.back().versionScope = "set";
                    context.state = SelectFromVersionSetBegin;
                } else if (token.equalsKeyword(Keyword::All)) {
                    // VERSION ALL [ DESCENDANTS | LEAVES OF b ]
                    context.selectStmt->relations.back().versionScope = "all";
                    context.state = SelectFromVersionAll;
                } else {
                    throw syntactical_error("Expected version name, '(' or 'ALL', found '" + token.value + "'");
                }
                break;
            case State::SelectFromDiffBegin:
            case State::SelectFromDiffSeparator:
                if (token.type == Type::identifier) {
                    Table &relation = context.selectStmt->relations.back();
                    if (relation.name.empty()) {
                        relation.name = token.value;
                    } else if (relation.name != token.value) {
                        throw syntactical_error("DIFF compares two versions of one table, found '" + relation.name + "' and '" + token.value + "'");
                    }
                    context.state = SelectFromDiffTable;
                } else {
                    throw syntactical_error("Expected table name, found '" + token.value + "'");
                }
                break;
            case State::SelectFromDiffTable:
            case State::SelectFromDiffName: {
                Table &relation = context.selectStmt->relations.back();
                if (context.state == State::SelectFromDiffTable && token.equalsKeyword(Keyword::Version)) {
                    context.state = SelectFromDiffVersion;
                    break;
                }
                if (context.state == State::SelectFromDiffTable) {
                    relation.versionSet.push_back(context.defaultBranch);
                }
                if (token.equalsControlSymbol(controlSymbols::separator) && relation.versionSet.size() == 1) {
                    context.state = SelectFromDiffSeparator;
                } else if (token.equalsControlSymbol(controlSymbols::closeBracket) && relation.versionSet.size() == 2) {
                    context.state = SelectFromTag;
                } else {
                    throw syntactical_error(std::string("Expected ") + (relation.versionSet.size() == 1 ? "','" : "')'") + ", found '" + token.value + "'");
                }
                break;
            }
            case State::SelectFromDiffVersion:
                if (token.type == Type::identifier) {
                    context.selectStmt->relations.back().versionSet.push_back(token.value);
                    context.state = SelectFromDiffName;
                } else {
                    throw syntactical_error("Expected version name, found '" + token.value + "'");
                }
                break;
            case State::SelectFromVersionSetBegin:
            case State::SelectFromVersionSetSeparator:
                if (token.type == Type::identifier) {
                    context.selectStmt->relations.back().versionSet.push_back(token.value);
                    context.state = SelectFromVersionSetName;
                } else {
                    throw syntactical_error("Expected version name, found '" + token.value + "'");
                }
                break;
            case State::SelectFromVersionSetName:
                if (token.equalsControlSymbol(controlSymbols::separator)) {
                    context.state = SelectFromVersionSetSeparator;
                } else if (token.equalsControlSymbol(controlSymbols::closeBracket)) {
                    context.state = SelectFromTag;
                } else {
                    throw syntactical_error("Expected ',' or ')', found '" + token.value + "'");
                }
                break;
            case State::SelectFromVersionAll:
                if (token.equalsKeyword(Keyword::Descendants) || token.equalsKeyword(Keyword::Leaves)) {
                    std::string scope;
                    std::transform(token.value.begin(), token.value.end(), std::back_inserter(scope), ::tolower);
                    context.selectStmt->relations.back().versionScope = scope;
                    context.state = SelectFromVersionAllScope;
                } else if (token.equalsKeyword(Keyword::Where)) {
                    context.state = State::SelectWhere;
                } else if (token.equalsKeyword(Keyword::Group)) {
                    context.state = State::SelectGroup;
                } else if (token.equalsKeyword(Keyword::Having)) {
                    context.state = State::SelectHaving;
                } else if (token.type == Type::identifier) {
                    context.selectStmt->relations.back().alias = token.value;
                    context.state = State::SelectFromBindingName;
                } else {
                    throw syntactical_error("Expected 'DESCENDANTS', 'LEAVES', binding name, 'WHERE' or 'GROUP', found '" + token.value + "'");
                }
                break;
            case State::SelectFromVersionAllScope:
                if (token.equalsKeyword(Keyword::Of)) {
                    context.state = SelectFromVersionAllOf;
                } else {
                    throw syntactical_error("Expected 'OF', found '" + token.value + "'");
                }
                break;
            case State::SelectFromVersionAllOf:
                if (token.type == Type::identifier) {
                    context.selectStmt->relations.back().version = token.value;
                    context.state = SelectFromTag;
                } else {
                    throw syntactical_error("Expected version name, found '" + token.value + "'");
                }
                break;
            case State::SelectFromTag:
                if (token.equalsKeyword(Keyword::Where)) {
                    context.state = State::SelectWhere;
                } else if (token.equalsKeyword(Keyword::Group)) {
                    context.state = State::SelectGroup;
                } else if (token.equalsKeyword(Keyword::Having)) {
                    context.state = State::SelectHaving;
                } else if (token.equalsControlSymbol(controlSymbols::separator)) {
                    context.state = State::SelectFromSeparator;
                } else if (token.type == Type::identifier) {
                    context.selectStmt->relations.back().alias = token.value;
                    context.state = State::SelectFromBindingName;
                } else {
                    throw syntactical_error("Expected binding name , 'WHERE' or ',', found '" + token.value + "'");
                }
                break;
            case State::SelectFromBindingName:
                if (token.equalsKeyword(Keyword::Where)) {
                    context.state = State::SelectWhere;
                } else if (token.equalsKeyword(Keyword::Group)) {
                    context.state = State::SelectGroup;
                } else if (token.equalsKeyword(Keyword::Having)) {
                    context.state = State::SelectHaving;
                } else if (token.equalsControlSymbol(controlSymbols::separator)) {
                    context.state = State::SelectFromSeparator;
                } else {
                    throw syntactical_error("Expected ',' or 'Where', found '" + token.value + "'");
                }
                break;
            case State::SelectWhere:
                switch (parseWhere(tokenizer, token, context.selectStmt->selections, context.selectStmt->predicates,
                                   &context.selectStmt->joinConditions, true)) {
                    case WhereEnd::Group: context.state = State::SelectGroup; break;
                    case WhereEnd::Having: context.state = State::SelectHaving; break;
                    case WhereEnd::End: context.state = State::Done; break;
                }
                break;
            case State::SelectWhereExprLhs:
                if (token.type == Type::op) {
                    context.state = State::SelectWhereExprOp;
                } else {
                    throw syntactical_error("Expected '=', found '" + token.value + "'");
                }
                break;
            case State::SelectWhereExprOp:
                if (token.type == Type::identifier) {
                    BindingAttribute lhs = parse_binding_attribute(tokenizer.prev(2).value);
                    BindingAttribute rhs = parse_binding_attribute(token.value);
                    context.selectStmt->joinConditions.push_back(std::make_pair(Column(),Column()));
                    context.selectStmt->joinConditions.back().first.table = lhs.first;
                    context.selectStmt->joinConditions.back().first.name = lhs.second;
                    context.selectStmt->joinConditions.back().second.table = rhs.first;
                    context.selectStmt->joinConditions.back().second.name = rhs.second;
                } else if (token.type == Type::literal) {
                    BindingAttribute lhs = parse_binding_attribute(tokenizer.prev(2).value);
                    context.selectStmt->selections.push_back(std::make_pair(Column(),token.value));
                    context.selectStmt->selections.back().first.table = lhs.first;
                    context.selectStmt->selections.back().first.name = lhs.second;
                } else {
                    throw syntactical_error("Expected right expression, found '" + token.value + "'");
                }
                context.state = State::SelectWhereExprRhs;
                break;
            case State::SelectWhereExprRhs:
                if (token.equalsKeyword(Keyword::And)) {
                    context.state = State::SelectWhereAnd;
                } else if (token.equalsKeyword(Keyword::Group)) {
                    context.state = State::SelectGroup;
                } else if (token.equalsKeyword(Keyword::Having)) {
                    context.state = State::SelectHaving;
                } else {
                    throw syntactical_error("Expected 'AND' or 'GROUP', found '" + token.value + "'");
                }
                break;
            case State::SelectGroup:
                if (token.equalsKeyword(Keyword::By)) {
                    context.state = State::SelectGroupBy;
                } else {
                    throw syntactical_error("Expected 'BY', found '" + token.value + "'");
                }
                break;
            case State::SelectGroupBy:
            case State::SelectGroupBySeparator:
                if (token.equalsKeyword(Keyword::Branch)) {
                    context.selectStmt->groupBy.push_back(Column());
                    context.selectStmt->groupBy.back().name = Keyword::Branch;
                    context.state = State::SelectGroupByName;
                } else if (token.type == Type::identifier) {
                    BindingAttribute columnBinding = parse_binding_attribute(token.value);
                    context.selectStmt->groupBy.push_back(Column());
                    context.selectStmt->groupBy.back().name = columnBinding.second;
                    context.selectStmt->groupBy.back().table = columnBinding.first;
                    context.state = State::SelectGroupByName;
                } else {
                    throw syntactical_error("Expected 'BRANCH' or column name, found '" + token.value + "'");
                }
                break;
            case State::SelectGroupByName:
                if (token.equalsControlSymbol(controlSymbols::separator)) {
                    context.state = State::SelectGroupBySeparator;
                } else if (token.equalsKeyword(Keyword::Having)) {
                    context.state = State::SelectHaving;
                } else {
                    throw syntactical_error("Expected ',', 'HAVING' or the end, found '" + token.value + "'");
                }
                break;
            case State::SelectHaving: {
                // HAVING condition: a condition over the groups, up to the end of the statement
                ExpressionParser parser(tokenizer, token);
                Node condition = parser.parse();
                if (!parser.token().hasType(Type::delimiter)) {
                    throw syntactical_error("Expected the end, found '" + parser.token().value + "'");
                }
                context.selectStmt->having = std::make_shared<Node>(std::move(condition));
                context.state = State::Done;
                break;
            }
            default:
                throw syntactical_error("unexpected token: " + token.value);
        }
    }

    void SQLParser::parseStatement(ParsingContext &context, std::string statement) {

        std::stringstream in(statement);
        // Initialize Tokenizer
        Tokenizer tokenizer(in);

        // Parse the input String token by token
        context.state = State::Init;
        while (context.state != State::Done) {
            parseToken(tokenizer, context);
        }
    }
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
// INSERT INTO professoren ( name , rang ) VALUES ( 'neumann' , 3 );
// create branch hello from master;
// insert into professoren version hello ( name , rang ) VALUES ( 'neumann' , 3 );
// select name from professoren version hello p;
// DELETE from professoren version hello p WHERE rang = 4;
// DELETE FROM professoren p WHERE rang = 4;

// CREATE TABLE studenten ( matrnr INTEGER NOT NULL , name VARCHAR ( 15 ) NOT NULL , semester INTEGER NOT NULL );
// CREATE TABLE hoeren ( matrnr INTEGER NOT NULL , vorlnr INTEGER NOT NULL );
// INSERT INTO studenten ( matrnr , name , semester ) VALUES ( 1 , 'student1' , 4 );
// INSERT INTO studenten ( matrnr , name , semester ) VALUES ( 2 , 'student2' , 5 );
// INSERT INTO hoeren ( matrnr , vorlnr ) VALUES ( 1 , 5 );
// INSERT INTO hoeren ( matrnr , vorlnr ) VALUES ( 2 , 5 );