// The expressions of the select list, of WHERE and of SET: types, casts and the functions and operators of arrays

#include "semanticAnalyser/SemanticAnalyser.hpp"

#include <climits>

#include "native/sql/SqlValues.hpp"

namespace semanticalAnalysis {

using sqlExpressions::Node;
using Kind = sqlExpressions::Node::Kind;
using Expressions::exp_op_t;
using Sql::SqlType;
using TypeID = Sql::SqlType::TypeID;
using Function = Native::Sql::Arrays::Function;

namespace {

bool isNullLiteral(const Node & n) { return n.kind == Kind::Literal && n.value == Native::Sql::nullLiteral; }
bool isLiteral(const Node & n) { return n.kind == Kind::Literal; }
bool isArray(SqlType t) { return t.typeID == TypeID::ArrayID; }
bool isNumber(SqlType t)
{
    return t.typeID == TypeID::IntegerID || t.typeID == TypeID::LongIntegerID || t.typeID == TypeID::NumericID || t.typeID == TypeID::DoubleID;
}
bool isString(SqlType t) { return t.typeID == TypeID::TextID || t.typeID == TypeID::VarcharID || t.typeID == TypeID::CharID; }

/// whether an array literal of constants (ARRAY[1, NULL, -2], also nested)
bool isConstantArray(const Node & n)
{
    if (n.kind != Kind::Array) return false;
    for (auto & c : n.children) if (!isLiteral(c) && !isConstantArray(c)) return false;
    return true;
}

std::string arrayText(const Node & array)
{
    std::string out = "{";
    for (size_t i = 0; i < array.children.size(); ++i) {
        const Node & e = array.children[i];
        if (i > 0) out += ",";
        if (e.kind == Kind::Array) {
            out += arrayText(e);
        } else if (isNullLiteral(e)) {
            out += "NULL";
        } else {
            out += '"';
            for (char c : e.value) {
                if (c == '"' || c == '\\') out += '\\';
                out += c;
            }
            out += '"';
        }
    }
    return out + "}";
}

/// The type of a literal without a context: a boolean, an integer (or bigint), a numeric, a double (with
/// an exponent), otherwise text
SqlType literalType(const Node & n)
{
    if (isNullLiteral(n)) return Sql::getUnknownTy();
    if (n.quoted) return Sql::getTextTy();
    const std::string & v = n.value;
    if (v == "true" || v == "false") return Sql::getBoolTy();
    size_t digits = 0, point = std::string::npos, exponent = std::string::npos;
    for (size_t i = 0; i < v.size(); ++i) {
        char c = v[i];
        if (std::isdigit(static_cast<unsigned char>(c))) ++digits;
        else if (c == '.' && point == std::string::npos && exponent == std::string::npos) point = i;
        else if ((c == 'e' || c == 'E') && exponent == std::string::npos && digits > 0) exponent = i;
        else if ((c == '-' || c == '+') && (i == 0 || (exponent != std::string::npos && i == exponent + 1))) continue;
        else return Sql::getTextTy();
    }
    if (digits == 0) return Sql::getTextTy();
    if (exponent != std::string::npos) return Sql::getDoubleTy();
    if (point != std::string::npos) {
        uint8_t scale = static_cast<uint8_t>(std::min<size_t>(v.size() - point - 1, 18));
        return Sql::getNumericTy(18, scale);
    }
    try {
        long long value = std::stoll(v);
        return (value >= INT32_MIN && value <= INT32_MAX) ? Sql::getIntegerTy() : Sql::getLongIntegerTy();
    } catch (const std::out_of_range &) {
        return Sql::getNumericTy(18, 0);
    }
}

int numberRank(SqlType t)
{
    switch (t.typeID) {
        case TypeID::IntegerID: return 1;
        case TypeID::LongIntegerID: return 2;
        case TypeID::NumericID: return 3;
        case TypeID::DoubleID: return 4;
        default: return 0;
    }
}

/// The type two numbers are computed in (not nullable)
SqlType commonNumber(SqlType a, SqlType b)
{
    a.nullable = b.nullable = false;
    if (a.typeID == TypeID::NumericID && b.typeID == TypeID::NumericID) {
        uint8_t scale = std::max(a.numericLayout.precision, b.numericLayout.precision);
        uint8_t integers = std::max(a.numericLayout.length - a.numericLayout.precision, b.numericLayout.length - b.numericLayout.precision);
        return Sql::getNumericTy(static_cast<uint8_t>(std::min(18, integers + scale)), scale);
    }
    return numberRank(a) >= numberRank(b) ? a : b;
}

/// The common type of the elements of an array constructor or of the arguments of a comparison
bool commonType(SqlType a, SqlType b, SqlType & result)
{
    if (a.typeID == TypeID::UnknownID) { result = Sql::toNotNullableTy(b); return true; }
    if (b.typeID == TypeID::UnknownID) { result = Sql::toNotNullableTy(a); return true; }
    if (isNumber(a) && isNumber(b)) { result = commonNumber(a, b); return true; }
    if (isString(a) && isString(b)) {
        result = Sql::equals(a, b, Sql::SqlTypeEqualsMode::WithoutNullable) ? Sql::toNotNullableTy(a) : Sql::getTextTy();
        return true;
    }
    if (Sql::equals(a, b, Sql::SqlTypeEqualsMode::WithoutNullable)) { result = Sql::toNotNullableTy(a); return true; }
    return false;
}

/// the name of PostgreSQL
std::string typeText(SqlType t)
{
    switch (t.typeID) {
        case TypeID::BoolID: return "boolean";
        case TypeID::IntegerID: return "integer";
        case TypeID::LongIntegerID: return "bigint";
        case TypeID::NumericID: return "numeric(" + std::to_string(t.numericLayout.length) + "," + std::to_string(t.numericLayout.precision) + ")";
        case TypeID::CharID: return "character(" + std::to_string(t.length) + ")";
        case TypeID::VarcharID: return "character varying(" + std::to_string(t.length) + ")";
        case TypeID::TextID: return "text";
        case TypeID::DateID: return "date";
        case TypeID::TimestampID: return "timestamp";
        case TypeID::DoubleID: return "double precision";
        case TypeID::ArrayID: return typeText(Sql::getElementTy(t)) + "[]";
        case TypeID::UnknownID: return "unknown";
    }
    return "unknown";
}

} // namespace

ExpressionAnalyser::ExpressionAnalyser(ColumnResolver resolve) : _resolve(std::move(resolve)) { }

exp_op_t ExpressionAnalyser::constant(const Node & n, SqlType type)
{
    if (isNullLiteral(n)) {
        if (type.typeID == TypeID::UnknownID) return std::make_unique<Expressions::NullConstant>();
        return std::make_unique<Expressions::Constant>(Native::Sql::nullLiteral, Sql::toNullableTy(type));
    }
    std::string text = n.kind == Kind::Array ? arrayText(n) : n.value;
    SqlType notNull = Sql::toNotNullableTy(type);
    try {
        Native::Sql::Value::castString(text, notNull); // fail here, not when the plan is translated
    } catch (const std::exception & e) {
        throw semantic_sql_error(std::string(e.what()) + " (the constant '" + text + "' as " + typeText(type) + ")");
    } catch (const char * e) {
        throw semantic_sql_error(std::string(e) + " (the constant '" + text + "' as " + typeText(type) + ")");
    }
    return std::make_unique<Expressions::Constant>(text, notNull);
}

exp_op_t ExpressionAnalyser::coerce(exp_op_t e, SqlType target, const std::string & what)
{
    SqlType type = e->getType();
    if (type.typeID == TypeID::UnknownID) {
        return std::make_unique<Expressions::Constant>(Native::Sql::nullLiteral, Sql::toNullableTy(target));
    }
    if (Sql::equals(type, target, Sql::SqlTypeEqualsMode::WithoutNullable)) return e;
    if (isNumber(type) && isNumber(target)) {
        return std::make_unique<Expressions::Cast>(std::move(e), Expressions::withNullability(target, type));
    }
    if (isString(type) && isString(target)) return e; // the runtime of arrays reads any string
    throw semantic_sql_error(what + " takes " + typeText(target) + ", not " + typeText(type));
}

exp_op_t ExpressionAnalyser::analyse(const Node & n, const SqlType * expected)
{
    switch (n.kind) {
        case Kind::Column: {
            iu_p_t iu = _resolve(n.table, n.name);
            if (iu == nullptr) throw semantic_sql_error("column '" + n.name + "' does not exist");
            return std::make_unique<Expressions::Identifier>(iu);
        }
        case Kind::Literal:
            return constant(n, expected ? *expected : literalType(n));
        case Kind::Array:
            return arrayConstructor(n, expected);
        case Kind::Subscript:
            return subscript(n);
        case Kind::Function:
            return function(n);
        case Kind::Operator:
            return operation(n);
        case Kind::Any: {
            // e op ANY (a): the type of the elements, from whichever side is not a constant
            exp_op_t element, array;
            if (!isLiteral(n.children[1]) || isLiteral(n.children[0])) {
                array = analyse(n.children[1]);
                if (!isArray(array->getType())) throw semantic_sql_error(n.value == "all" ? "ALL needs an array" : "ANY needs an array");
                SqlType elementType = Sql::getElementTy(array->getType());
                element = isLiteral(n.children[0]) ? analyse(n.children[0], &elementType) : analyse(n.children[0]);
            } else {
                element = analyse(n.children[0]);
                SqlType arrayType = Sql::getArrayTy(Sql::toNotNullableTy(element->getType()));
                array = analyse(n.children[1], &arrayType);
            }
            SqlType elementType = Sql::getElementTy(array->getType());
            element = coerce(std::move(element), elementType, "ANY");
            if (isNumber(element->getType()) && !Sql::equals(element->getType(), elementType, Sql::SqlTypeEqualsMode::WithoutNullable)) {
                throw semantic_sql_error("cannot compare " + typeText(element->getType()) + " with the elements of " + typeText(array->getType()));
            }
            static const std::map<std::string, int32_t> operators = {{"<", 0}, {"<=", 1}, {"=", 2}, {">=", 3}, {">", 4}, {"<>", 5}};
            Node code;
            code.kind = Kind::Literal;
            code.value = std::to_string(operators.at(n.name));
            std::vector<exp_op_t> arguments;
            arguments.push_back(std::move(element));
            arguments.push_back(std::move(array));
            arguments.push_back(constant(code, Sql::getIntegerTy()));
            return std::make_unique<Expressions::ArrayFunction>(n.value == "all" ? Function::AllCompare : Function::AnyCompare,
                                                                 Sql::getBoolTy(true), std::move(arguments));
        }
        case Kind::Between: {
            auto pair = typedPair(n.children[0], n.children[1], "BETWEEN");
            auto upper = typedPair(n.children[0], n.children[2], "BETWEEN");
            exp_op_t low = std::make_unique<Expressions::Comparison>(Expressions::ComparisonMode::geq, std::move(pair.first), std::move(pair.second));
            exp_op_t high = std::make_unique<Expressions::Comparison>(Expressions::ComparisonMode::leq, std::move(upper.first), std::move(upper.second));
            return std::make_unique<Expressions::And>(std::move(low), std::move(high));
        }
        case Kind::In: {
            exp_op_t result;
            for (size_t i = 1; i < n.children.size(); ++i) {
                auto pair = typedPair(n.children[0], n.children[i], "IN");
                exp_op_t eq = std::make_unique<Expressions::Comparison>(Expressions::ComparisonMode::eq, std::move(pair.first), std::move(pair.second));
                result = result ? std::make_unique<Expressions::Or>(std::move(result), std::move(eq)) : std::move(eq);
            }
            return result;
        }
        case Kind::Cast: {
            ColumnSpec spec;
            spec.type = n.name;
            spec.length = 0;
            spec.precision = 0;
            spec.nullable = false;
            if (!n.value.empty()) {
                auto comma = n.value.find(',');
                spec.length = std::stoul(n.value.substr(0, comma));
                if (comma != std::string::npos) spec.precision = std::stoul(n.value.substr(comma + 1));
            }
            if (!isColumnType(spec.type)) throw semantic_sql_error("type '" + spec.type + "' does not exist");
            bool array = spec.type.size() > 2 && spec.type.compare(spec.type.size() - 2, 2, "[]") == 0;
            std::string base = array ? spec.type.substr(0, spec.type.size() - 2) : spec.type;
            if ((base == "varchar" || base == "character varying") && spec.length == 0) spec.type = array ? "text[]" : "text";
            if ((base == "numeric" || base == "decimal") && spec.length == 0) {
                // numeric without a precision: that of a constant, otherwise 18 digits with 6 behind the point
                SqlType constantType = isLiteral(n.children[0]) ? literalType(n.children[0]) : Sql::getUnknownTy();
                spec.length = 18;
                spec.precision = constantType.typeID == TypeID::NumericID ? constantType.numericLayout.precision :
                                 constantType.typeID == TypeID::IntegerID || constantType.typeID == TypeID::LongIntegerID ? 0 : 6;
            }
            SqlType target = toColumnType(spec);
            const Node & value = n.children[0];
            if (isLiteral(value) || isConstantArray(value)) return constant(value, target);
            exp_op_t e = analyse(value);
            SqlType type = e->getType();
            if (type.typeID == TypeID::UnknownID) return coerce(std::move(e), target, "a cast");
            if (Sql::equals(type, target, Sql::SqlTypeEqualsMode::WithoutNullable)) return e;
            if (isNumber(type) && isNumber(target)) return coerce(std::move(e), target, "a cast");
            throw semantic_sql_error("cannot cast type " + typeText(type) + " to " + typeText(target));
        }
        case Kind::Row:
            throw semantic_sql_error("row values compare columns with constants only");
        default:
            throw semantic_sql_error("unexpected expression");
    }
}

std::pair<exp_op_t, exp_op_t> ExpressionAnalyser::typedPair(const Node & l, const Node & r, const std::string & what)
{
    // a constant (also ARRAY[ ... ] of constants) takes the type of the other side
    auto adaptable = [](const Node & c) { return isLiteral(c) || isConstantArray(c); };
    exp_op_t left, right;
    if (adaptable(l) && !adaptable(r)) {
        right = analyse(r);
        SqlType type = right->getType();
        left = analyse(l, &type);
    } else {
        left = analyse(l);
        SqlType type = left->getType();
        right = adaptable(r) && type.typeID != TypeID::UnknownID ? analyse(r, &type) : analyse(r);
    }
    SqlType a = left->getType(), b = right->getType(), common;
    if (!commonType(a, b, common)) {
        throw semantic_sql_error(what + " cannot compare " + typeText(a) + " with " + typeText(b));
    }
    if (isNumber(common) || a.typeID == TypeID::UnknownID || b.typeID == TypeID::UnknownID) {
        left = coerce(std::move(left), common, what);
        right = coerce(std::move(right), common, what);
    } else if (!Sql::equals(a, b, Sql::SqlTypeEqualsMode::WithoutNullable)) {
        throw semantic_sql_error(what + " cannot compare " + typeText(a) + " with " + typeText(b));
    }
    return {std::move(left), std::move(right)};
}

exp_op_t ExpressionAnalyser::arrayConstructor(const Node & n, const SqlType * expected)
{
    const bool expectArray = expected && isArray(*expected);
    if (isConstantArray(n)) {
        // a constant: the text of the array in the type of the context or of its elements
        SqlType type;
        if (expectArray) {
            type = Sql::toNotNullableTy(*expected);
        } else {
            SqlType element = Sql::getUnknownTy();
            std::function<void(const Node &)> visit = [&](const Node & a) {
                for (auto & c : a.children) {
                    if (c.kind == Kind::Array) { visit(c); continue; }
                    SqlType t = literalType(c);
                    if (!commonType(element, t, element)) throw semantic_sql_error("ARRAY types " + typeText(element) + " and " + typeText(t) + " cannot be matched");
                }
            };
            visit(n);
            if (element.typeID == TypeID::UnknownID) throw semantic_sql_error("cannot determine the type of an empty array (or of NULLs)");
            type = Sql::getArrayTy(element);
        }
        return constant(n, type);
    }
    // the elements (or arrays of one dimension less) in their common type
    const bool nested = !n.children.empty() && n.children.front().kind == Kind::Array;
    std::vector<exp_op_t> elements;
    SqlType element = expectArray ? Sql::getElementTy(*expected) : Sql::getUnknownTy();
    if (!expectArray) {
        for (auto & c : n.children) {
            if (isLiteral(c) || isConstantArray(c)) continue;
            exp_op_t e = analyse(c);
            SqlType t = nested ? (isArray(e->getType()) ? Sql::getElementTy(e->getType()) : e->getType()) : e->getType();
            if (!commonType(element, t, element)) throw semantic_sql_error("ARRAY types " + typeText(element) + " and " + typeText(t) + " cannot be matched");
        }
        if (element.typeID == TypeID::UnknownID) {
            for (auto & c : n.children) if (isLiteral(c) && !commonType(element, literalType(c), element)) throw semantic_sql_error("ARRAY types do not match");
        }
        if (element.typeID == TypeID::UnknownID) throw semantic_sql_error("cannot determine the type of the array");
    }
    element = Sql::toNotNullableTy(element);
    SqlType arrayType = Sql::getArrayTy(element);
    for (auto & c : n.children) {
        if ((c.kind == Kind::Array) != nested) throw semantic_sql_error("multidimensional arrays must have array expressions with matching dimensions");
        if (nested) {
            exp_op_t sub = analyse(c, &arrayType);
            if (!isArray(sub->getType())) throw semantic_sql_error("an ARRAY of arrays takes arrays");
            elements.push_back(std::move(sub));
        } else {
            exp_op_t e = analyse(c, isLiteral(c) ? &element : nullptr);
            elements.push_back(coerce(std::move(e), element, "ARRAY"));
        }
    }
    return std::make_unique<Expressions::ArrayFunction>(Function::Construct, arrayType, std::move(elements));
}

exp_op_t ExpressionAnalyser::integerArgument(const Node & n, const std::string & what)
{
    SqlType integer = Sql::getIntegerTy();
    exp_op_t e = analyse(n, isLiteral(n) ? &integer : nullptr);
    if (!isNumber(e->getType()) && e->getType().typeID != TypeID::UnknownID) throw semantic_sql_error(what + " takes an integer");
    if (e->getType().typeID == TypeID::DoubleID || e->getType().typeID == TypeID::NumericID) throw semantic_sql_error(what + " takes an integer");
    return coerce(std::move(e), integer, what);
}

exp_op_t ExpressionAnalyser::subscript(const Node & n)
{
    exp_op_t array = analyse(n.children[0]);
    SqlType type = array->getType();
    if (!isArray(type)) throw semantic_sql_error("cannot subscript type " + typeText(type) + ", as it is not an array");
    bool slice = false;
    for (size_t i = 1; i < n.children.size(); ++i) slice = slice || n.children[i].kind == Kind::Slice;
    std::vector<exp_op_t> arguments;
    arguments.push_back(std::move(array));
    auto bound = [&](const Node & b, int32_t omitted) -> exp_op_t {
        if (b.kind == Kind::Omitted) {
            Node literal;
            literal.kind = Kind::Literal;
            literal.value = std::to_string(omitted);
            return constant(literal, Sql::getIntegerTy());
        }
        return integerArgument(b, "an array subscript");
    };
    for (size_t i = 1; i < n.children.size(); ++i) {
        const Node & index = n.children[i];
        if (!slice) {
            arguments.push_back(integerArgument(index, "an array subscript"));
        } else if (index.kind == Kind::Slice) {
            arguments.push_back(bound(index.children[0], INT32_MIN));
            arguments.push_back(bound(index.children[1], INT32_MAX));
        } else {
            // a subscript among slices is the slice from 1
            Node one;
            one.kind = Kind::Literal;
            one.value = "1";
            arguments.push_back(constant(one, Sql::getIntegerTy()));
            arguments.push_back(integerArgument(index, "an array subscript"));
        }
    }
    if (slice) return std::make_unique<Expressions::ArrayFunction>(Function::Slice, Sql::toNullableTy(type), std::move(arguments));
    return std::make_unique<Expressions::ArrayFunction>(Function::Subscript, Sql::getElementTy(type), std::move(arguments));
}

/// An array argument and element arguments of a polymorphic function: the array type from whichever
/// is not a constant; constants take it
exp_op_t ExpressionAnalyser::polymorphic(const std::string & name, const std::vector<const Node *> & arrays,
                                         const std::vector<const Node *> & elements, std::vector<exp_op_t> & arrayOut,
                                         std::vector<exp_op_t> & elementOut, SqlType & arrayType)
{
    arrayType = Sql::getUnknownTy();
    std::vector<exp_op_t> analysedArrays(arrays.size()), analysedElements(elements.size());
    for (size_t i = 0; i < arrays.size(); ++i) {
        if (isLiteral(*arrays[i]) || isConstantArray(*arrays[i])) continue;
        analysedArrays[i] = analyse(*arrays[i]);
        SqlType t = analysedArrays[i]->getType();
        if (!isArray(t)) throw semantic_sql_error(name + " takes an array, not " + typeText(t));
        if (arrayType.typeID == TypeID::UnknownID) arrayType = Sql::toNotNullableTy(t);
        else if (!Sql::equals(arrayType, t, Sql::SqlTypeEqualsMode::WithoutNullable)) {
            throw semantic_sql_error(name + " takes arrays of one type, not " + typeText(arrayType) + " and " + typeText(t));
        }
    }
    if (arrayType.typeID == TypeID::UnknownID) {
        for (size_t i = 0; i < elements.size(); ++i) {
            if (isLiteral(*elements[i])) continue;
            analysedElements[i] = analyse(*elements[i]);
            arrayType = Sql::getArrayTy(Sql::toNotNullableTy(analysedElements[i]->getType()));
            break;
        }
    }
    if (arrayType.typeID == TypeID::UnknownID) {
        // constants only: the elements' own type, or that of a constant array
        for (auto * e : elements) if (!isNullLiteral(*e)) { arrayType = Sql::getArrayTy(literalType(*e)); break; }
        for (auto * a : arrays) if (arrayType.typeID == TypeID::UnknownID && isConstantArray(*a)) arrayType = arrayConstructor(*a, nullptr)->getType();
    }
    if (arrayType.typeID == TypeID::UnknownID) throw semantic_sql_error("could not determine the type of the arguments of " + name);
    SqlType elementType = Sql::toNotNullableTy(Sql::getElementTy(arrayType));
    for (size_t i = 0; i < arrays.size(); ++i) {
        if (!analysedArrays[i]) analysedArrays[i] = analyse(*arrays[i], &arrayType);
        arrayOut.push_back(std::move(analysedArrays[i]));
    }
    for (size_t i = 0; i < elements.size(); ++i) {
        if (!analysedElements[i]) analysedElements[i] = analyse(*elements[i], isLiteral(*elements[i]) ? &elementType : nullptr);
        elementOut.push_back(coerce(std::move(analysedElements[i]), elementType, name));
    }
    return nullptr;
}

exp_op_t ExpressionAnalyser::textArgument(const Node & n, const std::string & what)
{
    SqlType text = Sql::getTextTy();
    exp_op_t e = analyse(n, isLiteral(n) ? &text : nullptr);
    if (!isString(e->getType()) && e->getType().typeID != TypeID::UnknownID) throw semantic_sql_error(what + " takes text, not " + typeText(e->getType()));
    if (e->getType().typeID == TypeID::UnknownID) return coerce(std::move(e), text, what);
    return e;
}

exp_op_t ExpressionAnalyser::function(const Node & n)
{
    const std::string & name = n.name;
    const size_t argc = n.children.size();
    auto arity = [&](size_t low, size_t high) {
        if (argc < low || argc > high) throw semantic_sql_error("function " + name + " takes " + std::to_string(low) +
                                                                (high > low ? " to " + std::to_string(high) : "") + " arguments");
    };
    auto call = [](Function f, SqlType type, std::vector<exp_op_t> arguments) -> exp_op_t {
        return std::make_unique<Expressions::ArrayFunction>(f, type, std::move(arguments));
    };
    auto arrayOnly = [&](const Node & a) {
        std::vector<exp_op_t> arrays, elements;
        SqlType type;
        polymorphic(name, {&a}, {}, arrays, elements, type);
        return std::move(arrays.front());
    };
    if (name == "count" || name == "sum" || name == "min" || name == "max" || name == "avg") {
        throw semantic_sql_error("the aggregate " + name + " takes a column here, as in " + name + "(column)");
    }
    if (name == "cardinality" || name == "array_ndims" || name == "array_dims") {
        arity(1, 1);
        std::vector<exp_op_t> arguments;
        arguments.push_back(arrayOnly(n.children[0]));
        if (name == "array_dims") return call(Function::Dims, Sql::getTextTy(true), std::move(arguments));
        return call(name == "cardinality" ? Function::Cardinality : Function::Ndims, Sql::getIntegerTy(true), std::move(arguments));
    }
    if (name == "array_length" || name == "array_lower" || name == "array_upper") {
        arity(2, 2);
        std::vector<exp_op_t> arguments;
        arguments.push_back(arrayOnly(n.children[0]));
        arguments.push_back(integerArgument(n.children[1], name));
        Function f = name == "array_length" ? Function::Length : name == "array_lower" ? Function::Lower : Function::Upper;
        return call(f, Sql::getIntegerTy(true), std::move(arguments));
    }
    if (name == "trim_array") {
        arity(2, 2);
        std::vector<exp_op_t> arguments;
        arguments.push_back(arrayOnly(n.children[0]));
        SqlType type = Sql::toNullableTy(arguments.front()->getType());
        arguments.push_back(integerArgument(n.children[1], name));
        return call(Function::Trim, type, std::move(arguments));
    }
    if (name == "array_append" || name == "array_prepend" || name == "array_remove" || name == "array_replace" ||
        name == "array_position" || name == "array_positions" || name == "array_cat") {
        std::vector<const Node *> arrays, elements;
        if (name == "array_append" || name == "array_remove" || name == "array_positions") { arity(2, 2); arrays = {&n.children[0]}; elements = {&n.children[1]}; }
        else if (name == "array_prepend") { arity(2, 2); elements = {&n.children[0]}; arrays = {&n.children[1]}; }
        else if (name == "array_replace") { arity(3, 3); arrays = {&n.children[0]}; elements = {&n.children[1], &n.children[2]}; }
        else if (name == "array_position") { arity(2, 3); arrays = {&n.children[0]}; elements = {&n.children[1]}; }
        else { arity(2, 2); arrays = {&n.children[0], &n.children[1]}; }
        std::vector<exp_op_t> arrayArgs, elementArgs, arguments;
        SqlType type;
        polymorphic(name, arrays, elements, arrayArgs, elementArgs, type);
        if (name == "array_prepend") {
            arguments.push_back(std::move(elementArgs[0]));
            arguments.push_back(std::move(arrayArgs[0]));
        } else {
            for (auto & a : arrayArgs) arguments.push_back(std::move(a));
            for (auto & e : elementArgs) arguments.push_back(std::move(e));
        }
        SqlType result = Sql::toNullableTy(type);
        if (name == "array_append") return call(Function::Append, result, std::move(arguments));
        if (name == "array_prepend") return call(Function::Prepend, result, std::move(arguments));
        if (name == "array_cat") return call(Function::Cat, result, std::move(arguments));
        if (name == "array_remove") return call(Function::Remove, result, std::move(arguments));
        if (name == "array_replace") return call(Function::Replace, result, std::move(arguments));
        if (name == "array_positions") return call(Function::Positions, Sql::getArrayTy(Sql::getIntegerTy(), true), std::move(arguments));
        if (argc == 3) arguments.push_back(integerArgument(n.children[2], name));
        return call(Function::Position, Sql::getIntegerTy(true), std::move(arguments));
    }
    if (name == "array_fill") {
        arity(2, 3);
        std::vector<exp_op_t> arguments;
        if (isNullLiteral(n.children[0])) throw semantic_sql_error("could not determine the type of the elements of array_fill");
        arguments.push_back(analyse(n.children[0]));
        SqlType element = Sql::toNotNullableTy(arguments.front()->getType());
        SqlType integers = Sql::getArrayTy(Sql::getIntegerTy());
        for (size_t i = 1; i < argc; ++i) {
            exp_op_t dims = analyse(n.children[i], &integers);
            if (!Sql::equals(dims->getType(), integers, Sql::SqlTypeEqualsMode::WithoutNullable)) throw semantic_sql_error("array_fill takes integer[] dimensions");
            arguments.push_back(std::move(dims));
        }
        return call(Function::Fill, Sql::getArrayTy(element, true), std::move(arguments));
    }
    if (name == "array_to_string") {
        arity(2, 3);
        std::vector<exp_op_t> arguments;
        arguments.push_back(arrayOnly(n.children[0]));
        for (size_t i = 1; i < argc; ++i) arguments.push_back(textArgument(n.children[i], name));
        return call(Function::ToString, Sql::getTextTy(true), std::move(arguments));
    }
    if (name == "string_to_array") {
        arity(2, 3);
        std::vector<exp_op_t> arguments;
        for (size_t i = 0; i < argc; ++i) arguments.push_back(textArgument(n.children[i], name));
        return call(Function::FromString, Sql::getArrayTy(Sql::getTextTy(), true), std::move(arguments));
    }
    throw semantic_sql_error("function " + name + " does not exist");
}

exp_op_t ExpressionAnalyser::operation(const Node & n)
{
    const std::string & op = n.name;
    SqlType boolean = Sql::getBoolTy();
    auto condition = [&](const Node & c) {
        exp_op_t e = analyse(c, isLiteral(c) ? &boolean : nullptr);
        if (e->getType().typeID != TypeID::BoolID && e->getType().typeID != TypeID::UnknownID) {
            throw semantic_sql_error("argument of " + (op == "not" ? std::string("NOT") : op == "and" ? std::string("AND") : std::string("OR")) +
                                     " must be type boolean, not " + typeText(e->getType()));
        }
        return coerce(std::move(e), boolean, op);
    };
    if (op == "and") return std::make_unique<Expressions::And>(condition(n.children[0]), condition(n.children[1]));
    if (op == "or") return std::make_unique<Expressions::Or>(condition(n.children[0]), condition(n.children[1]));
    if (op == "not") return std::make_unique<Expressions::Not>(condition(n.children[0]));
    if (op == "is null" || op == "is not null") return std::make_unique<Expressions::IsNull>(analyse(n.children[0]), op == "is not null");
    if (op == "neg") {
        exp_op_t e = analyse(n.children[0]);
        if (!isNumber(e->getType())) throw semantic_sql_error("unary minus takes a number");
        Node zero;
        zero.kind = Kind::Literal;
        zero.value = "0";
        return std::make_unique<Expressions::Subtraction>(constant(zero, e->getType()), std::move(e));
    }
    if (op == "+" || op == "-" || op == "*" || op == "/") {
        auto pair = typedPair(n.children[0], n.children[1], "operator " + op);
        if (!isNumber(pair.first->getType())) throw semantic_sql_error("operator " + op + " takes numbers, not " + typeText(pair.first->getType()));
        if (op == "+") return std::make_unique<Expressions::Addition>(std::move(pair.first), std::move(pair.second));
        if (op == "-") return std::make_unique<Expressions::Subtraction>(std::move(pair.first), std::move(pair.second));
        if (op == "*") return std::make_unique<Expressions::Multiplication>(std::move(pair.first), std::move(pair.second));
        return std::make_unique<Expressions::Division>(std::move(pair.first), std::move(pair.second));
    }
    if (op == "=" || op == "<>" || op == "<" || op == "<=" || op == ">" || op == ">=") {
        auto pair = typedPair(n.children[0], n.children[1], "operator " + op);
        using Mode = Expressions::ComparisonMode;
        Mode mode = op == "<" ? Mode::less : op == "<=" ? Mode::leq : op == ">" ? Mode::gtr : op == ">=" ? Mode::geq : Mode::eq;
        exp_op_t comparison = std::make_unique<Expressions::Comparison>(mode, std::move(pair.first), std::move(pair.second));
        if (op == "<>") return std::make_unique<Expressions::Not>(std::move(comparison));
        return comparison;
    }
    if (op == "@>" || op == "<@" || op == "&&") {
        std::vector<exp_op_t> arrays, elements;
        SqlType type;
        polymorphic("operator " + op, {&n.children[0], &n.children[1]}, {}, arrays, elements, type);
        Function f = op == "@>" ? Function::Contains : op == "<@" ? Function::ContainedBy : Function::Overlaps;
        return std::make_unique<Expressions::ArrayFunction>(f, Sql::getBoolTy(true), std::move(arrays));
    }
    if (op == "||") {
        // array || array, array || element, element || array: by the types, a quoted constant beside an array is an array
        const Node & l = n.children[0], & r = n.children[1];
        auto arrayLike = [&](const Node & c, const Node & other) {
            if (c.kind == Kind::Array) return true;
            if (isLiteral(c)) return c.quoted && !c.value.empty() && (c.value[0] == '{' || c.value[0] == '[');
            (void) other;
            return isArray(analyse(c)->getType());
        };
        bool leftArray = arrayLike(l, r), rightArray = arrayLike(r, l);
        if (!leftArray && !rightArray) throw semantic_sql_error("operator || takes an array (TardisDB has no concatenation of text yet)");
        std::vector<exp_op_t> arrayArgs, elementArgs, arguments;
        SqlType type;
        if (leftArray && rightArray) {
            polymorphic("operator ||", {&l, &r}, {}, arrayArgs, elementArgs, type);
            return std::make_unique<Expressions::ArrayFunction>(Function::Cat, Sql::toNullableTy(type), std::move(arrayArgs));
        }
        if (leftArray) {
            polymorphic("operator ||", {&l}, {&r}, arrayArgs, elementArgs, type);
            arguments.push_back(std::move(arrayArgs[0]));
            arguments.push_back(std::move(elementArgs[0]));
            return std::make_unique<Expressions::ArrayFunction>(Function::Append, Sql::toNullableTy(type), std::move(arguments));
        }
        polymorphic("operator ||", {&r}, {&l}, arrayArgs, elementArgs, type);
        arguments.push_back(std::move(elementArgs[0]));
        arguments.push_back(std::move(arrayArgs[0]));
        return std::make_unique<Expressions::ArrayFunction>(Function::Prepend, Sql::toNullableTy(type), std::move(arguments));
    }
    throw semantic_sql_error("operator " + op + " does not exist");
}

exp_op_t ExpressionAnalyser::analyseCondition(const Node & n)
{
    SqlType boolean = Sql::getBoolTy();
    exp_op_t e = analyse(n, isLiteral(n) ? &boolean : nullptr);
    if (e->getType().typeID != TypeID::BoolID) {
        throw semantic_sql_error("argument of WHERE must be type boolean, not " + typeText(e->getType()));
    }
    return e;
}

exp_op_t ExpressionAnalyser::analyseValue(const Node & n, SqlType target, const std::string & column)
{
    exp_op_t e = analyse(n, isLiteral(n) || n.kind == Kind::Array ? &target : nullptr);
    SqlType type = e->getType();
    if (type.typeID == TypeID::UnknownID) return coerce(std::move(e), target, "column '" + column + "'");
    if (isNumber(type) && isNumber(target)) return e; // the update casts
    if (!Sql::equals(type, target, Sql::SqlTypeEqualsMode::WithoutNullable)) {
        throw semantic_sql_error("column '" + column + "' is of type " + typeText(target) + ", but the expression is of type " + typeText(type));
    }
    return e;
}

} // namespace semanticalAnalysis
