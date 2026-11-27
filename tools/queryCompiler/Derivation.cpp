#include "queryCompiler/Derivation.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <sstream>
#include <vector>

#include "runtime/SqlValues.hpp"
#include "semanticAnalyser/SemanticAnalyser.hpp"
#include "sqlParser/SQLParser.hpp"

namespace Derivation {

using sqlExpressions::Node;
using Kind = Node::Kind;
using semanticalAnalysis::semantic_sql_error;

namespace {

Node make(Kind kind, const std::string & name, std::vector<Node> children)
{
    Node n;
    n.kind = kind;
    n.name = name;
    n.children = std::move(children);
    return n;
}

Node number(const std::string & text)
{
    Node n;
    n.kind = Kind::Literal;
    n.value = text;
    return n;
}

Node op(const std::string & name, std::vector<Node> children) { return make(Kind::Operator, name, std::move(children)); }

Node call(const std::string & name, std::vector<Node> children) { return make(Kind::Function, name, std::move(children)); }

/// \returns Whether the node is a number literal, with its value
bool isNumber(const Node & n, double & value)
{
    if (n.kind != Kind::Literal || n.quoted || n.value.empty()) return false;
    char * end = nullptr;
    value = std::strtod(n.value.c_str(), &end);
    return *end == '\0';
}

bool isOne(const Node & n)
{
    double v;
    return isNumber(n, v) && v == 1;
}

std::string numberText(double v)
{
    if (v == std::floor(v) && std::fabs(v) < 1e15) return std::to_string(static_cast<long long>(v));
    std::ostringstream out;
    out.precision(17);
    out << v;
    return out.str();
}

Node mul(Node a, Node b)
{
    if (isOne(a)) return b;
    if (isOne(b)) return a;
    return op("*", {std::move(a), std::move(b)});
}

Node div(Node a, Node b)
{
    if (isOne(b)) return a;
    return op("/", {std::move(a), std::move(b)});
}

Node neg(Node a)
{
    if (a.kind == Kind::Operator && a.name == "neg") return std::move(a.children.front());
    return op("neg", {std::move(a)});
}

Node sub(Node a, Node b) { return op("-", {std::move(a), std::move(b)}); }

/// \returns base ^ exponent, simplified for the exponents 0 and 1
Node power(Node base, Node exponent)
{
    double v;
    if (isNumber(exponent, v) && v == 1) return base;
    if (isNumber(exponent, v) && v == 0) return number("1");
    return op("^", {std::move(base), std::move(exponent)});
}

bool equalsIgnoreCase(const std::string & a, const std::string & b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return ::tolower(x) == ::tolower(y); });
}

/// Reverse mode: each node passes the derivative of the expression by its value (the seed) on to its operands
class Differentiator {
public:
    explicit Differentiator(std::string parameter) : _parameter(std::move(parameter)) { }

    void derive(const Node & e, const Node & seed);

    std::vector<std::string> columns; // in the order of their first use
    std::map<std::string, Node> derivatives;

private:
    std::string _parameter;

    bool isParameter(const Node & n) const { return n.kind == Kind::Column && equalsIgnoreCase(n.table, _parameter); }

    bool depends(const Node & n) const
    {
        if (isParameter(n)) return true;
        return std::any_of(n.children.begin(), n.children.end(), [&](const Node & c) { return depends(c); });
    }

    /// Derives into the operand \p i of \p e with the seed, if it depends on the parameter
    void into(const Node & e, size_t i, const Node & seed)
    {
        if (depends(e.children[i])) derive(e.children[i], seed);
    }

    [[noreturn]] static void unsupported(const std::string & what)
    {
        throw semantic_sql_error("derivation: no derivative of " + what);
    }
};

void Differentiator::derive(const Node & e, const Node & seed)
{
    if (!depends(e)) return;
    if (isParameter(e)) {
        auto it = derivatives.find(e.name);
        if (it == derivatives.end()) {
            columns.push_back(e.name);
            derivatives.emplace(e.name, seed);
        } else {
            it->second = op("+", {std::move(it->second), seed});
        }
        return;
    }
    const auto & c = e.children;
    if (e.kind == Kind::Cast) {
        into(e, 0, seed);
        return;
    }
    if (e.kind == Kind::Operator) {
        const std::string & o = e.name;
        if (o == "+") {
            into(e, 0, seed);
            into(e, 1, seed);
        } else if (o == "-" && c.size() == 2) {
            into(e, 0, seed);
            into(e, 1, neg(seed));
        } else if (o == "neg" || (o == "-" && c.size() == 1)) {
            into(e, 0, neg(seed));
        } else if (o == "*") {
            into(e, 0, mul(seed, c[1]));
            into(e, 1, mul(seed, c[0]));
        } else if (o == "/") {
            into(e, 0, div(seed, c[1]));
            into(e, 1, neg(div(mul(seed, c[0]), mul(c[1], c[1]))));
        } else if (o == "^") {
            // r l^(r-1) and l^r ln(l)
            double r;
            Node lowered = isNumber(c[1], r) ? number(numberText(r - 1)) : sub(c[1], number("1"));
            into(e, 0, mul(seed, mul(c[1], power(c[0], lowered))));
            into(e, 1, mul(seed, mul(e, call("ln", {c[0]}))));
        } else if (o == "**") {
            // the seed in the shape of the product, then S r^T and l^T S
            Node shaped = call("unbroadcast", {seed, e});
            into(e, 0, op("**", {shaped, call("transpose", {c[1]})}));
            into(e, 1, op("**", {call("transpose", {c[0]}), shaped}));
        } else if (o == "@") {
            into(e, 0, mul(seed, sub(call("relup", {c[0]}), call("relup", {neg(c[0])}))));
        } else {
            unsupported("operator " + o);
        }
        return;
    }
    if (e.kind != Kind::Function) unsupported(Derivation::toSql(e));
    const std::string & f = e.name;
    auto unary = [&](Node derivative) { into(e, 0, std::move(derivative)); };
    if (f == "sig") {
        Node s = call("sig", {c[0]});
        unary(mul(seed, mul(s, sub(number("1"), s))));
    } else if (f == "tanh") {
        unary(mul(seed, sub(number("1"), op("^", {call("tanh", {c[0]}), number("2")}))));
    } else if (f == "relu") {
        unary(mul(seed, call("relup", {c[0]})));
    } else if (f == "abs") {
        unary(mul(seed, sub(call("relup", {c[0]}), call("relup", {neg(c[0])}))));
    } else if (f == "exp") {
        unary(mul(seed, call("exp", {c[0]})));
    } else if (f == "ln") {
        unary(div(seed, c[0]));
    } else if (f == "log" && c.size() == 1) {
        unary(div(seed, mul(c[0], call("ln", {number("10")}))));
    } else if (f == "log" && c.size() == 2) {
        // log(b, x): 1 / (x ln b) and -ln x / (b ln^2 b)
        Node lnBase = call("ln", {c[0]});
        into(e, 1, div(seed, mul(c[1], lnBase)));
        into(e, 0, neg(div(mul(seed, call("ln", {c[1]})), mul(c[0], op("^", {lnBase, number("2")})))));
    } else if (f == "sqrt") {
        unary(div(seed, mul(number("2"), call("sqrt", {c[0]}))));
    } else if (f == "sin") {
        unary(mul(seed, call("cos", {c[0]})));
    } else if (f == "cos") {
        unary(neg(mul(seed, call("sin", {c[0]}))));
    } else if (f == "softmax") {
        // s (S - sum(S s))
        Node s = call("softmax", {c[0]});
        unary(mul(s, sub(seed, call("unbroadcast", {mul(seed, s), number("0")}))));
    } else if (f == "softmaxcce" && c.size() == 2) {
        into(e, 0, mul(seed, call("softmaxccep", {c[0], c[1]})));
        into(e, 1, neg(mul(seed, call("ln", {call("softmax", {c[0]})}))));
    } else if (f == "transpose" && c.size() == 1) {
        unary(call("transpose", {call("unbroadcast", {seed, e})}));
    } else {
        unsupported("function " + f);
    }
}

std::string quote(const std::string & text)
{
    std::string out = "'";
    for (char ch : text) out += ch == '\'' ? std::string("''") : std::string(1, ch);
    return out + "'";
}

std::string join(const std::vector<Node> & nodes, size_t from = 0)
{
    std::string out;
    for (size_t i = from; i < nodes.size(); ++i) out += (i > from ? ", " : "") + toSql(nodes[i]);
    return out;
}

std::string upper(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), ::toupper);
    return text;
}

/// The elements of an array constructor, nested ones without ARRAY
std::string elements(const Node & array)
{
    std::string out = "[";
    for (size_t i = 0; i < array.children.size(); ++i) {
        const Node & c = array.children[i];
        out += (i ? ", " : "") + (c.kind == Kind::Array ? elements(c) : toSql(c));
    }
    return out + "]";
}

} // namespace

std::string toSql(const Node & n)
{
    const auto & c = n.children;
    switch (n.kind) {
        case Kind::Column: return n.table.empty() ? n.name : n.table + "." + n.name;
        case Kind::Literal:
            if (n.value == Native::Sql::nullLiteral) return "NULL";
            if (n.quoted) return quote(n.value);
            return !n.value.empty() && n.value[0] == '-' ? "(" + n.value + ")" : n.value;
        case Kind::Array: return "ARRAY" + elements(n);
        case Kind::Subscript: {
            std::string out = toSql(c[0]);
            for (size_t i = 1; i < c.size(); ++i) {
                if (c[i].kind != Kind::Slice) { out += "[" + toSql(c[i]) + "]"; continue; }
                auto bound = [](const Node & b) { return b.kind == Kind::Omitted ? std::string() : toSql(b); };
                out += "[" + bound(c[i].children[0]) + ":" + bound(c[i].children[1]) + "]";
            }
            return out;
        }
        case Kind::Slice:
        case Kind::Omitted: break;
        case Kind::Function: return n.name + "(" + (n.value == "*" ? std::string("*") : join(c)) + ")";
        case Kind::Operator:
            if (c.size() == 1) {
                if (n.name == "neg") return "(- " + toSql(c[0]) + ")";
                if (n.name == "is null" || n.name == "is not null") return "(" + toSql(c[0]) + " " + upper(n.name) + ")";
                return "(" + upper(n.name) + " " + toSql(c[0]) + ")";
            }
            return "(" + toSql(c[0]) + " " + upper(n.name) + " " + toSql(c[1]) + ")";
        case Kind::Any: return "(" + toSql(c[0]) + " " + n.name + " " + upper(n.value) + " (" + toSql(c[1]) + "))";
        case Kind::Row: return "(" + join(c) + ")";
        case Kind::Between: return "(" + toSql(c[0]) + " BETWEEN " + toSql(c[1]) + " AND " + toSql(c[2]) + ")";
        case Kind::In: return "(" + toSql(c[0]) + " IN (" + join(c, 1) + "))";
        case Kind::Cast: {
            std::string type = n.name;
            if (!n.value.empty()) type.insert(std::min(type.find('['), type.size()), "(" + n.value + ")");
            return "CAST(" + toSql(c[0]) + " AS " + type + ")";
        }
    }
    throw semantic_sql_error("derivation: cannot write the expression");
}

std::string query(const std::string & table, const std::string & parameter, const std::string & expression)
{
    const Node e = tardisParser::SQLParser::parseExpression(expression);
    Differentiator differentiator(parameter);
    differentiator.derive(e, number("1"));
    std::string select = "SELECT *";
    for (const std::string & column : differentiator.columns) {
        // in the shape of the column: summed where it met arrays, repeated where it is one
        Node shaped = call("unbroadcast", {differentiator.derivatives.at(column), make(Kind::Column, column, {})});
        shaped.children[1].table = parameter;
        select += ", " + toSql(shaped) + " AS d_" + column;
    }
    return select + " FROM (" + table + ") " + parameter;
}

} // end namespace Derivation
