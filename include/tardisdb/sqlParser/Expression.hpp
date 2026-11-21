#pragma once

#include <memory>
#include <string>
#include <vector>

namespace sqlExpressions {

/// An expression of the select list, of WHERE or of SET, as the parser reads it (shared by the parser's
/// and the semantic analyser's statements, whose layouts must match)
struct Node {
    enum class Kind : unsigned char {
        Column,     // table (binding, may be empty) and name
        Literal,    // value: a number, a string (quoted), true or false; NULL is Native::Sql::nullLiteral
        Array,      // ARRAY[children]: elements, or arrays (ARRAY[[1,2],[3,4]])
        Subscript,  // children[0], then per dimension an index or a Slice
        Slice,      // lower:upper; children: the bounds, each maybe an Omitted
        Omitted,    // an omitted bound of a slice
        Function,   // name (lower case) of a call with the children as arguments; count(*): no children, value "*"
        Operator,   // name: + - * / || @> <@ && = <> < <= > >= and or not neg ("is null", "is not null")
        Any,        // name: the comparison, value: "any" or "all"; children: the element and the array
        Row,        // ( a , b ): a row value
        Between,    // children: the value, the lower and the upper bound
        In,         // children: the value, then the items
        Cast,       // name: the type ("numeric", "integer[]"), value: its length and precision ("10,2"); children: the value
    } kind = Kind::Literal;
    std::string name;     // of a column, function or operator
    std::string table;    // the binding of a column
    std::string value;    // the text of a literal
    bool quoted = false;  // a string literal ('5' is a string, 5 a number)
    std::vector<Node> children;
};

using node_p_t = std::shared_ptr<Node>;

/// The relation of a SELECT without FROM: a temporary table of one row (TemporaryTables), under a name
/// that no statement can spell
inline const std::string oneRowTable("\x01one");

} // end namespace sqlExpressions
