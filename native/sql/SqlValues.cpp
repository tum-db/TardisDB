#include "native/sql/SqlValues.hpp"
#include "codegen/LLVMCompat.hpp"
#include "codegen/CodeGen.hpp"

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <limits>

#include "foundations/exceptions.hpp"
#include "foundations/LegacyTypes.hpp"
#include "foundations/StringPool.hpp"
#include "sql/SqlType.hpp"
#include "utils/general.hpp"

//using Sql::SqlType;

using namespace HashUtils;

namespace Native {
namespace Sql {

//-----------------------------------------------------------------------------
// Value

Value::Value(SqlType type) :
        type(type)
{ }

Value::~Value()
{ }

value_op_t Value::castString(const std::string & str, SqlType type)
{
    assert(!type.nullable);

    switch (type.typeID) {
        case SqlType::TypeID::UnknownID:
            return UnknownValue::create();
        case SqlType::TypeID::BoolID:
            return Bool::castString(str);
        case SqlType::TypeID::IntegerID:
            return Integer::castString(str);
        case SqlType::TypeID::VarcharID:
        case SqlType::TypeID::CharID:
            // as the generated code, a char of a length above 1 is stored as a varchar
            return Varchar::castString(str,type);
        case SqlType::TypeID::LongIntegerID: {
            // no native LongInteger: a Numeric holds the 64 bits, the type says what they are
            auto value = std::make_unique<Numeric>(type);
            size_t end = 0;
            value->value = std::stoll(str, &end);
            if (end != str.size()) throw InvalidOperationException("invalid longinteger '" + str + "'");
            return value;
        }
        case SqlType::TypeID::NumericID:
            return Numeric::castString(str, type);
        case SqlType::TypeID::DateID:
            return Date::castString(str);
        case SqlType::TypeID::TimestampID:
            return Timestamp::castString(str);
        case SqlType::TypeID::TextID:
            return Text::castString(str);
        case SqlType::TypeID::DoubleID:
            return Double::castString(str);
        case SqlType::TypeID::ArrayID:
            return Array::castString(str, type);
        default:
            unreachable();
    }
}

value_op_t Value::load(const void * ptr, SqlType type)
{
    if (type.nullable) {
        return NullableValue::load(ptr, type);
    }

    switch (type.typeID) {
        case SqlType::TypeID::UnknownID:
            return UnknownValue::create();
        case SqlType::TypeID::BoolID:
            return Bool::load(ptr);
        case SqlType::TypeID::IntegerID:
            return Integer::load(ptr);
        case SqlType::TypeID::VarcharID:
        case SqlType::TypeID::CharID:
            return Varchar::load(ptr,type);
        case SqlType::TypeID::LongIntegerID: {
            auto value = std::make_unique<Numeric>(type);
            memcpy(&value->value, ptr, sizeof(int64_t));
            return value;
        }
        case SqlType::TypeID::NumericID:
            return Numeric::load(ptr, type);
        case SqlType::TypeID::DateID:
            return Date::load(ptr);
        case SqlType::TypeID::TimestampID:
            return Timestamp::load(ptr);
        case SqlType::TypeID::TextID:
            return Text::load(ptr);
        case SqlType::TypeID::DoubleID:
            return Double::load(ptr);
        case SqlType::TypeID::ArrayID:
            return Array::load(ptr, type);
        default:
            throw InvalidOperationException("unknown type");
    }
}

size_t Value::getSize() const
{
    return getValueSize(type);
}

//-----------------------------------------------------------------------------
// Integer

Integer::Integer() :
    Value(::Sql::getIntegerTy()),
    value(0)
{ }

Integer::Integer(Sql::SqlType type) : Value(type), value(0)
{}

Integer::Integer(const void * src) :
    Value(::Sql::getIntegerTy()),
    value(*static_cast<const value_type *>(src))
{ }

Integer::Integer(value_type constantValue) :
    Value(::Sql::getIntegerTy()),
    value(constantValue)
{ }

value_op_t Integer::clone() const
{
    Value * cloned = new Integer(value);
    return value_op_t(cloned);
}

value_op_t Integer::castString(const std::string & str)
{
    Integer::value_type value = castStringToIntegerValue(str.c_str(), static_cast<uint32_t>(str.length()));
    value_op_t sqlValue( new Integer(value) );
    return sqlValue;
}

value_op_t Integer::load(const void * ptr)
{
    value_op_t sqlValue( new Integer(ptr) );
    return sqlValue;
}

void Integer::store(void * ptr) const
{
    *static_cast<value_type *>(ptr) = value;
}

hash_t Integer::hash() const
{
    return hashInteger(value);
}
/*
void Integer::accept(ValueVisitor & visitor)
{
    visitor.visit(*this);
}
*/
bool Integer::equals(const Value & other) const
{
    if (!::Sql::equals(type, other.type, ::Sql::SqlTypeEqualsMode::WithoutNullable)) {
        return false;
    }

    if (isNullable(other)) {
        return other.equals(*this);
    }

    return (value == dynamic_cast<const Integer &>(other).value);
}

bool Integer::compare(const Value & other, ComparisonMode mode) const
{
    throw NotImplementedException();
}

//-----------------------------------------------------------------------------
// Numeric

Numeric::Numeric() :
    Value(::Sql::getNumericFullLengthTy(2)),
    value(0)
{ }

        Numeric::Numeric(Sql::SqlType type) : Value(type), value(0)
        {}

Numeric::Numeric(const void * src, SqlType type) :
    Value(type),
    value(*static_cast<const value_type *>(src))
{
    assert(type.typeID == SqlType::TypeID::NumericID && !type.nullable);
}

Numeric::Numeric(SqlType type, value_type constantValue) :
        Value(type),
        value(constantValue)
{
    assert(type.typeID == SqlType::TypeID::NumericID && !type.nullable);
}

value_op_t Numeric::clone() const
{
    Value * cloned = new Numeric(type, value);
    return value_op_t(cloned);
}

value_op_t Numeric::castString(const std::string & str, SqlType type)
{
    int64_t value = castStringToNumericValue(
            str.c_str(),
            static_cast<uint32_t>(str.size()),
            type.numericLayout.length,
            type.numericLayout.precision
    );
    value_op_t sqlValue( new Numeric(type, value) );
    return sqlValue;
}

value_op_t Numeric::load(const void * ptr, SqlType type)
{
    value_op_t sqlValue( new Numeric(ptr, type) );
    return sqlValue;
}

void Numeric::store(void * ptr) const
{
    *static_cast<value_type *>(ptr) = value;
}

hash_t Numeric::hash() const
{
    // can be used for all integral types
    return hashInteger(value);
}
/*
void Numeric::accept(ValueVisitor & visitor)
{
    visitor.visit(*this);
}
*/
bool Numeric::equals(const Value & other) const
{
    if (!::Sql::equals(type, other.type, ::Sql::SqlTypeEqualsMode::WithoutNullable)) {
        return false;
    }

    if (isNullable(other)) {
        return other.equals(*this);
    }

    return (value == dynamic_cast<const Numeric &>(other).value);
}

bool Numeric::compare(const Value & other, ComparisonMode mode) const
{
    throw NotImplementedException();
}

//-----------------------------------------------------------------------------
// Bool

Bool::Bool() :
    Value(::Sql::getBoolTy()),
    value(false)
{ }

        Bool::Bool(Sql::SqlType type) : Value(type), value(false)
        {}

Bool::Bool(const void * src) :
    Value(::Sql::getBoolTy()),
    value(*static_cast<const value_type *>(src))
{ }


Bool::Bool(value_type constantValue) :
    Value(::Sql::getBoolTy()),
    value(constantValue)
{ }

value_op_t Bool::clone() const
{
    Value * cloned = new Bool(value);
    return value_op_t(cloned);
}

value_op_t Bool::castString(const std::string & str)
{
    // the spellings of PostgreSQL, in any case and with surrounding spaces
    std::string s;
    for (char c : str) if (!std::isspace(static_cast<unsigned char>(c))) s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    bool value;
    if (s == "t" || s == "true" || s == "y" || s == "yes" || s == "on" || s == "1") {
        value = true;
    } else if (s == "f" || s == "false" || s == "n" || s == "no" || s == "off" || s == "0") {
        value = false;
    } else {
        throw InvalidOperationException("invalid input syntax for type boolean: \"" + str + "\"");
    }

    value_op_t sqlValue( new Bool(value) );
    return sqlValue;
}

value_op_t Bool::load(const void * ptr)
{
    value_op_t sqlValue( new Bool(ptr) );
    return sqlValue;
}

void Bool::store(void * ptr) const
{
    *static_cast<value_type *>(ptr) = value;
}

hash_t Bool::hash() const
{
    return hashInteger(value);
}
/*
void Bool::accept(ValueVisitor & visitor)
{
    visitor.visit(*this);
}
*/
bool Bool::equals(const Value & other) const
{
    if (!::Sql::equals(type, other.type, ::Sql::SqlTypeEqualsMode::WithoutNullable)) {
        return false;
    }

    if (isNullable(other)) {
        return other.equals(*this);
    }

    return (value == dynamic_cast<const Bool &>(other).value);
}

bool Bool::compare(const Value & other, ComparisonMode mode) const
{
    throw NotImplementedException();
}

//-----------------------------------------------------------------------------
// Date

Date::Date() :
    Value(::Sql::getDateTy()),
    value(0)
{ }

        Date::Date(Sql::SqlType type) : Value(type), value(0)
        {}

Date::Date(const void * src) :
    Value(::Sql::getDateTy()),
    value(*static_cast<const value_type *>(src))
{ }

Date::Date(value_type constantValue) :
    Value(::Sql::getDateTy()),
    value(constantValue)
{ }

value_op_t Date::clone() const
{
    Value * cloned = new Date(value);
    return value_op_t(cloned);
}

value_op_t Date::castString(const std::string & str)
{
    value_type value = castStringToDateValue(str.c_str(), static_cast<uint32_t>(str.length()));
    value_op_t sqlValue( new Date(value) );
    return sqlValue;
}

value_op_t Date::load(const void * ptr)
{
    value_op_t sqlValue( new Date(ptr) );
    return sqlValue;
}

void Date::store(void * ptr) const
{
    *static_cast<value_type *>(ptr) = value;
}

hash_t Date::hash() const
{
    // can be used for all integral types
    return hashInteger(value);
}
/*
void Date::accept(ValueVisitor & visitor)
{
    visitor.visit(*this);
}
*/
bool Date::equals(const Value & other) const
{
    if (!::Sql::equals(type, other.type, ::Sql::SqlTypeEqualsMode::WithoutNullable)) {
        return false;
    }

    if (isNullable(other)) {
        return other.equals(*this);
    }

    return (value == dynamic_cast<const Date &>(other).value);
}

bool Date::compare(const Value & other, ComparisonMode mode) const
{
    throw NotImplementedException();
}

//-----------------------------------------------------------------------------
// Timestamp

Timestamp::Timestamp() :
    Value(::Sql::getTimestampTy()),
    value(0)
{ }

        Timestamp::Timestamp(Sql::SqlType type) : Value(type), value(0)
        {}

Timestamp::Timestamp(const void * src) :
    Value(::Sql::getTimestampTy()),
    value(*static_cast<const value_type *>(src))
{ }

Timestamp::Timestamp(value_type constantValue) :
    Value(::Sql::getTimestampTy()),
    value(constantValue)
{ }

value_op_t Timestamp::clone() const
{
    Value * cloned = new Timestamp(value);
    return value_op_t(cloned);
}

value_op_t Timestamp::castString(const std::string & str)
{
    value_type value = castStringToTimestampValue(str.c_str(), static_cast<uint32_t>(str.length()));
    value_op_t sqlValue( new Timestamp(value) );
    return sqlValue;
}

value_op_t Timestamp::load(const void * ptr)
{
    value_op_t sqlValue( new Timestamp(ptr) );
    return sqlValue;
}

void Timestamp::store(void * ptr) const
{
    *static_cast<value_type *>(ptr) = value;
}

hash_t Timestamp::hash() const
{
    // can be used for all integral types
    return hashInteger(value);
}
/*
void Timestamp::accept(ValueVisitor & visitor)
{
    visitor.visit(*this);
}
*/
bool Timestamp::equals(const Value & other) const
{
    if (!::Sql::equals(type, other.type, ::Sql::SqlTypeEqualsMode::WithoutNullable)) {
        return false;
    }

    if (isNullable(other)) {
        return other.equals(*this);
    }

    return (value == dynamic_cast<const Timestamp &>(other).value);
}

bool Timestamp::compare(const Value & other, ComparisonMode mode) const
{
    throw NotImplementedException();
}

//-----------------------------------------------------------------------------
// Double

Double::Double(value_type constantValue) :
    Value(::Sql::getDoubleTy()),
    value(constantValue)
{ }

/// \returns The shortest digits of \p value that read back exactly, in scientific notation (e.g. 1.5e+00)
static std::string shortestScientific(double value)
{
    char buffer[64];
#if defined(__cpp_lib_to_chars)
    auto result = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::scientific);
    return std::string(buffer, result.ptr);
#else
    // before GCC 11 (e.g. GCC 9 of the container), to_chars has no floating point
    for (int precision = 0; precision < 17; ++precision) {
        std::snprintf(buffer, sizeof(buffer), "%.*e", precision, value);
        if (std::strtod(buffer, nullptr) == value) return buffer;
    }
    std::snprintf(buffer, sizeof(buffer), "%.16e", value);
    return buffer;
#endif
}

std::string formatDouble(double value)
{
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return value > 0 ? "Infinity" : "-Infinity";
    if (value == 0) return std::signbit(value) ? "-0" : "0";
    // the shortest digits that read back exactly, laid out as PostgreSQL does
    std::string scientific = shortestScientific(value);
    size_t e = scientific.find('e');
    std::string mantissa = scientific.substr(0, e);
    int exponent = std::stoi(scientific.substr(e + 1));
    bool negative = mantissa[0] == '-';
    std::string digits;
    for (char c : mantissa) if (std::isdigit(static_cast<unsigned char>(c))) digits += c;
    std::string out = negative ? "-" : "";
    if (exponent < -4 || exponent >= 15) {
        out += digits.substr(0, 1);
        if (digits.size() > 1) out += "." + digits.substr(1);
        out += exponent < 0 ? "e-" : "e+";
        std::string ex = std::to_string(std::abs(exponent));
        if (ex.size() < 2) ex.insert(0, "0");
        return out + ex;
    }
    if (exponent < 0) {
        return out + "0." + std::string(static_cast<size_t>(-exponent - 1), '0') + digits;
    }
    size_t integerDigits = static_cast<size_t>(exponent) + 1;
    if (digits.size() <= integerDigits) return out + digits + std::string(integerDigits - digits.size(), '0');
    return out + digits.substr(0, integerDigits) + "." + digits.substr(integerDigits);
}

double parseDouble(const std::string & str)
{
    size_t begin = 0, end = str.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(str[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(str[end - 1]))) --end;
    std::string s = str.substr(begin, end - begin);
    std::string lower;
    for (char c : s) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower == "nan") return std::numeric_limits<double>::quiet_NaN();
    if (lower == "infinity" || lower == "+infinity" || lower == "inf" || lower == "+inf") return std::numeric_limits<double>::infinity();
    if (lower == "-infinity" || lower == "-inf") return -std::numeric_limits<double>::infinity();
    bool valid = !s.empty() && lower.find_first_not_of("0123456789+-.e") == std::string::npos;
    char * stop = nullptr;
    errno = 0;
    double value = valid ? std::strtod(s.c_str(), &stop) : 0;
    if (!valid || stop != s.c_str() + s.size()) {
        throw std::invalid_argument("invalid input syntax for type double precision: \"" + str + "\"");
    }
    if (errno == ERANGE && std::isinf(value)) {
        throw std::invalid_argument("\"" + str + "\" is out of range for type double precision");
    }
    return value;
}

std::string formatNumeric(int64_t raw, unsigned scale)
{
    uint64_t magnitude = raw < 0 ? 0 - static_cast<uint64_t>(raw) : static_cast<uint64_t>(raw);
    std::string digits = std::to_string(magnitude);
    if (scale > 0) {
        if (digits.size() <= scale) digits.insert(0, scale - digits.size() + 1, '0');
        digits.insert(digits.size() - scale, ".");
    }
    return (raw < 0 ? "-" : "") + digits;
}

value_op_t Double::castString(const std::string & str)
{
    return value_op_t(new Double(parseDouble(str)));
}

value_op_t Double::load(const void * ptr)
{
    double v;
    std::memcpy(&v, ptr, sizeof(v));
    return value_op_t(new Double(v));
}

value_op_t Double::clone() const
{
    return value_op_t(new Double(value));
}

void Double::store(void * ptr) const
{
    std::memcpy(ptr, &value, sizeof(value));
}

hash_t Double::hash() const
{
    if (std::isnan(value)) return hashInteger<uint64_t>(0x7ff8000000000000ull);
    double v = value + 0.0; // -0 hashes as 0
    uint64_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    return hashInteger(bits);
}

static int compareDoubles(double a, double b)
{
    bool aNaN = std::isnan(a), bNaN = std::isnan(b);
    if (aNaN || bNaN) return aNaN == bNaN ? 0 : (aNaN ? 1 : -1);
    return a < b ? -1 : (a > b ? 1 : 0);
}

bool Double::equals(const Value & other) const
{
    if (!::Sql::equals(type, other.type, ::Sql::SqlTypeEqualsMode::WithoutNullable)) {
        return false;
    }
    if (isNullable(other)) {
        return other.equals(*this);
    }
    return compareDoubles(value, dynamic_cast<const Double &>(other).value) == 0;
}

bool Double::compare(const Value & other, ComparisonMode mode) const
{
    int c = compareDoubles(value, dynamic_cast<const Double &>(getAssociatedValue(other)).value);
    switch (mode) {
        case ComparisonMode::less: return c < 0;
        case ComparisonMode::leq: return c <= 0;
        case ComparisonMode::eq: return c == 0;
        case ComparisonMode::geq: return c >= 0;
        case ComparisonMode::gtr: return c > 0;
    }
    return false;
}

//-----------------------------------------------------------------------------
// Array

Array::Array(SqlType type, Arrays::Handle handle) :
    Value(type),
    value(handle)
{ }

value_op_t Array::castString(const std::string & str, SqlType type)
{
    return value_op_t(new Array(type, Arrays::parse(str, type, nullptr)));
}

value_op_t Array::load(const void * ptr, SqlType type)
{
    auto * array = new Array(type, Arrays::loadHandle(ptr));
    array->ownTemporary(); // the arena of the query may go before the value
    return value_op_t(array);
}

void Array::ownTemporary()
{
    if (!value.isTemporary() || (_owned && reinterpret_cast<const uint8_t *>(_owned->data()) == value.data)) return;
    _owned = std::make_shared<const std::string>(reinterpret_cast<const char *>(value.data), value.size());
    value = Arrays::Handle{reinterpret_cast<const uint8_t *>(_owned->data()), value.size() | Arrays::Handle::temporary};
}

value_op_t Array::clone() const
{
    auto * array = new Array(type, value);
    array->_owned = _owned;
    return value_op_t(array);
}

void Array::store(void * ptr) const
{
    Arrays::storeHandle(ptr, Arrays::persist(value));
}

hash_t Array::hash() const
{
    return Arrays::hash(value, type);
}

bool Array::equals(const Value & other) const
{
    if (!::Sql::equals(type, other.type, ::Sql::SqlTypeEqualsMode::WithoutNullable)) {
        return false;
    }
    if (isNullable(other)) {
        return other.equals(*this);
    }
    return Arrays::equals(value, dynamic_cast<const Array &>(other).value, type);
}

bool Array::compare(const Value & other, ComparisonMode mode) const
{
    int c = Arrays::compare(value, dynamic_cast<const Array &>(getAssociatedValue(other)).value, type);
    switch (mode) {
        case ComparisonMode::less: return c < 0;
        case ComparisonMode::leq: return c <= 0;
        case ComparisonMode::eq: return c == 0;
        case ComparisonMode::geq: return c >= 0;
        case ComparisonMode::gtr: return c > 0;
    }
    return false;
}

std::string Array::toString() const
{
    return Arrays::format(value, type);
}

//-----------------------------------------------------------------------------
// Text

Text::Text() :
    Value(::Sql::getTextTy())
{
    value[0] = 0;
    value[1] = 0;
}

        Text::Text(Sql::SqlType type) : Value(type)
        {
            value[0] = 0;
            value[1] = 0;
        }

Text::Text(const void * src) :
    Value(::Sql::getTextTy())
{
    const uintptr_t * src_array = reinterpret_cast<const uintptr_t *>(src);
    value[0] = src_array[0];
    value[1] = src_array[1];
}

Text::Text(const uint8_t * beginPtr, const uint8_t * endPtr) :
    Value(::Sql::getTextTy())
{
    uintptr_t first_value = reinterpret_cast<uintptr_t>(beginPtr);
    first_value ^= static_cast<uintptr_t>(1) << (8*sizeof(uintptr_t)-1);
    value[0] = first_value;
    value[1] = reinterpret_cast<uintptr_t>(endPtr);
}

Text::Text(uint8_t len, const uint8_t * bytes) :
    Value(::Sql::getTextTy())
{
    assert(len <= 15);
    uint8_t * data = reinterpret_cast<uint8_t *>(value.data());
    data[0] = len;
    std::memcpy(&data[1], bytes, len);
}

value_op_t Text::clone() const
{
    Value * cloned = new Text(value.data());
    return value_op_t(cloned);
}

value_op_t Text::castString(const std::string & str)
{
    const uint8_t * bytes = reinterpret_cast<const uint8_t *>(str.c_str());
    size_t len = str.size();
    if (len > 15) {
        std::unique_ptr<uint8_t[]> data(new uint8_t[len]);
        std::memcpy(data.get(), bytes, len);
        auto & storedStr = StringPool::instance().put(sql_string_t(len, std::move(data)));
        uint8_t * beginPtr = storedStr.second.get();
        uint8_t * endPtr = beginPtr+len;
        value_op_t sqlValue( new Text(beginPtr, endPtr) );
        return sqlValue;
    } else {
        value_op_t sqlValue( new Text(static_cast<uint8_t>(len), bytes) );
        return sqlValue;
    }
}

value_op_t Text::load(const void * ptr)
{
    value_op_t sqlValue( new Text(ptr));
    return sqlValue;
}

void Text::store(void * ptr) const
{
    uintptr_t * dst_array = reinterpret_cast<uintptr_t *>(ptr);
    dst_array[0] = value[0];
    dst_array[1] = value[1];
}

hash_t Text::hash() const
{
    size_t len = length();
    return hashByteArray(begin(), len);
}
/*
void Text::accept(ValueVisitor & visitor)
{
    visitor.visit(*this);
}
*/
bool Text::equals(const Value & other) const
{
    if (!::Sql::equals(type, other.type, ::Sql::SqlTypeEqualsMode::WithoutNullable)) {
        return false;
    }

    if (isNullable(other)) {
        return other.equals(*this);
    }

    size_t len = length();
    auto & otherText = dynamic_cast<const Text &>(other);
    if (len != otherText.length()) {
        return false;
    }

    if (isInplace()) {
        return std::equal(value.begin(), value.end(), otherText.value.begin());
    } else {
        const void * buf1 = begin();
        const void * buf2 = otherText.begin();
        return (0 == std::memcmp(buf1, buf2, len));
    }
}

bool Text::compare(const Value & other, ComparisonMode mode) const
{
    throw NotImplementedException();
}

const uint8_t * Text::begin() const {
    if (isInplace()) {
        const uint8_t * data = reinterpret_cast<const uint8_t *>(value.data());
        return &data[1];
    } else {
        uintptr_t begin = value[0];
        // untag the leftmost bit
        begin ^= static_cast<uintptr_t>(1) << (8*sizeof(uintptr_t)-1);
        uint8_t * beginPtr = reinterpret_cast<uint8_t *>(begin);
        return beginPtr;
    }
/*
    mov p1, data[0]
    mov p2, &data[0] + 1
    mov mask1, p1
    sar 63, mask1
    and p1, 2^63 - 1
    mov mask2, mask1
    not mask2
    and p1, mask1
    and p2, mask2
    or p1, p2
    return p1
*/
}

Text::string_ref_t Text::getString() const {
    return std::make_pair(length(), begin());
}

std::string_view Text::getView() const {
    return std::string_view(reinterpret_cast<const char *>(begin()), length());
}

bool Text::isInplace() const {
    bool inplace = (0 == (value[0] >> (8*sizeof(uintptr_t)-1)));
    return inplace;
}

size_t Text::length() const {
    if (isInplace()) {
        const uint8_t * data = reinterpret_cast<const uint8_t *>(value.data());
        return data[0];
    } else {
        // remove tag
        uintptr_t beginValue = reinterpret_cast<uintptr_t>(begin());
        ptrdiff_t diff = value[1] - beginValue;
        return diff;
    }
/*
    mov p1, data[0]
    mov mask1, p1
    sar 63, mask1
    mov mask2, mask1
    not mask2
    mov size1, data[1] // TODO store the string length directly in data[1]
    sub size1, p1
    sal 56, p1 // size2
    and size1, mask1
    and size2, mask2
    or size1, size2
    return size1
*/
}

value_op_t Varchar::castString(const std::string &str, Sql::SqlType type) {
    // a longer value would be written past its slot
    if (str.size() > type.length) {
        throw InvalidOperationException("value too long for type " + std::string(type.typeID == Sql::SqlType::TypeID::CharID ? "char" : "varchar") +
                                        "(" + std::to_string(type.length) + ")");
    }
    Varchar* result = new Varchar(type);
    result->value = str.c_str(); // the string outlives the value: it belongs to the statement
    result->len = str.size();

    return value_op_t(result);
}

// the layout of the generated code: a length of 1, 2 or 4 bytes by the capacity, then the characters
static size_t varcharLengthBytes(size_t capacity)
{
    return (capacity < 256) ? 1 : (capacity < 65536) ? 2 : 4;
}

static uint32_t loadVarcharLength(const void * ptr, size_t capacity)
{
    switch (varcharLengthBytes(capacity)) {
        case 1: return *reinterpret_cast<const uint8_t *>(ptr);
        case 2: { uint16_t l; memcpy(&l, ptr, 2); return l; }
        default: { uint32_t l; memcpy(&l, ptr, 4); return l; }
    }
}

value_op_t Varchar::load(const void *ptr, Sql::SqlType type) {
    Varchar* result = new Varchar(type);
    result->load(ptr);
    return value_op_t(result);
}

void Varchar::store(void *ptr) const {
    if (type.typeID == Sql::SqlType::TypeID::CharID && type.length == 1) {
        // char(1) is the character itself
        *reinterpret_cast<char *>(ptr) = len > 0 ? value[0] : ' ';
        return;
    }
    size_t lengthBytes = varcharLengthBytes(type.length);
    switch (lengthBytes) {
        case 1: *reinterpret_cast<uint8_t *>(ptr) = static_cast<uint8_t>(len); break;
        case 2: { uint16_t l = static_cast<uint16_t>(len); memcpy(ptr, &l, 2); break; }
        default: memcpy(ptr, &len, 4); break;
    }
    memcpy((void*)(reinterpret_cast<char*>(ptr) + lengthBytes),value,len);
}

void Varchar::load(const void * ptr) {
    if (type.typeID == Sql::SqlType::TypeID::CharID && type.length == 1) {
        value = reinterpret_cast<const char*>(ptr);
        len = 1;
        return;
    }
    value = reinterpret_cast<const char*>(ptr) + varcharLengthBytes(type.length);
    len = loadVarcharLength(ptr, type.length);
}

        value_op_t Varchar::clone() const
        {
            Varchar * cloned = new Varchar(type);
            cloned->value = value;
            cloned->len = len;
            return value_op_t(cloned);
        }


        hash_t Varchar::hash() const
        {
            return hashByteArray(begin(), len);
        }
/*
void Varchar::accept(ValueVisitor & visitor)
{
    visitor.visit(*this);
}
*/
        bool Varchar::equals(const Value & other) const
        {
            if (!::Sql::equals(type, other.type, ::Sql::SqlTypeEqualsMode::WithoutNullable)) {
                return false;
            }

            if (isNullable(other)) {
                return other.equals(*this);
            }

            size_t len = length();
            auto & otherText = dynamic_cast<const Varchar &>(other);
            if (len != otherText.length()) {
                return false;
            }

            const void * buf1 = begin();
            const void * buf2 = otherText.begin();
            return (0 == std::memcmp(buf1, buf2, len));
        }

        bool Varchar::compare(const Value & other, ComparisonMode mode) const
        {
            throw NotImplementedException();
        }

//-----------------------------------------------------------------------------
// UnknownValue

UnknownValue::UnknownValue() :
        Value(::Sql::getUnknownTy())
{ }

value_op_t UnknownValue::create()
{
    return value_op_t(new UnknownValue);
}

void UnknownValue::store(void * ptr) const
{
    // nop
}

hash_t UnknownValue::hash() const
{
    // arbitrary constant
    return 0x2c8e4eecfc696c50ul;
}

value_op_t UnknownValue::clone() const
{
    return create();
}
/*
void UnknownValue::accept(ValueVisitor & visitor)
{
    visitor.visit(*this);
}
*/
bool UnknownValue::equals(const Value & other) const
{
    return false;
}

bool UnknownValue::compare(const Value & other, ComparisonMode mode) const
{
    return false;
}

//-----------------------------------------------------------------------------
// NullableValue (with external null indicator)


/// NullableValue (with external null indicator)
NullableValue::NullableValue(value_op_t sqlValue, bool nullIndicator) :
        Value( ::Sql::toNullableTy( sqlValue->type ) ),
        _sqlValue(std::move(sqlValue)),
        _nullIndicator(nullIndicator)
{
//    Functions::genPrintfCall("NullableValue ctor nullable %d\n", _nullIndicator.llvmValue);
    assert(!_sqlValue->type.nullable && type.typeID != SqlType::TypeID::UnknownID);
    assert(dynamic_cast<NullableValue *>(_sqlValue.get()) == nullptr); // do not nest NullableValues
}

/// \returns The offset of the null indicator in a value of the nullable type
static size_t nullIndicatorOffset(SqlType type)
{
    auto & dataLayout = getThreadLocalCodeGen().getDefaultDataLayout();
    auto * structTy = llvm::cast<llvm::StructType>(toLLVMTy(toNullableTy(type)));
    return dataLayout.getStructLayout(structTy)->getElementOffset(1);
}

/// \returns Zeroed bytes of at least the given size, which live as long as the process: values such
/// as Varchar point into the bytes they were loaded from
static const uint8_t * zeroedBytes(size_t size)
{
    static std::mutex latch;
    static std::map<size_t, std::unique_ptr<uint8_t[]>> buffers;
    std::lock_guard<std::mutex> lock(latch);
    auto & buffer = buffers[size];
    if (!buffer) buffer = std::make_unique<uint8_t[]>(size); // value-initialized: zeroed
    return buffer.get();
}

value_op_t NullableValue::getNull(SqlType type)
{
    SqlType notNullableType = toNotNullableTy(type);
    const size_t size = getThreadLocalCodeGen().getDefaultDataLayout().getTypeAllocSize(toLLVMTy(notNullableType));
    return create(Value::load(zeroedBytes(size), notNullableType), true);
}

value_op_t NullableValue::create(value_op_t value, bool nullIndicator)
{
    return value_op_t( new NullableValue(std::move(value), nullIndicator) );
}

value_op_t NullableValue::create(const Value & value, bool nullIndicator)
{
    const Value & assoc = getAssociatedValue(value);
    return create(assoc.clone(), nullIndicator);
}

value_op_t NullableValue::load(const void * ptr, SqlType type)
{
    assert(type.nullable);
    const bool isNull = static_cast<const uint8_t *>(ptr)[nullIndicatorOffset(type)] != 0;
    return create(Value::load(ptr, toNotNullableTy(type)), isNull);
    /*
    assert(type.nullable);

    auto & codeGen = getThreadLocalCodeGen();
    llvm::Type * valueTy = Sql::toLLVMTy(type);

    llvm::Value * innerValuePtr = codeGen->CreateStructGEP(valueTy, ptr, 0);
    SqlType notNullableType = Sql::toNotNullableTy(type);
    value_op_t sqlValue = Value::load(innerValuePtr, notNullableType);

    llvm::Value * nullIndicatorPtr = codeGen->CreateStructGEP(valueTy, ptr, 1);
    cg_bool_t nullIndicator( codeGen->CreateLoad(cg_bool_t::getType(), nullIndicatorPtr) );

    return NullableValue::create(std::move(sqlValue), cg_bool_t(nullIndicator));
    */
}

value_op_t NullableValue::clone() const
{
    value_op_t cloned = _sqlValue->clone();
    return create(std::move(cloned), _nullIndicator);
}

void NullableValue::store(void * ptr) const
{
    const size_t offset = nullIndicatorOffset(type);
    if (_nullIndicator) {
        // a NULL stores the zeroed value, so that the bytes of two NULLs are equal
        std::memset(ptr, 0, offset);
    } else {
        _sqlValue->store(ptr);
    }
    static_cast<uint8_t *>(ptr)[offset] = _nullIndicator ? 1 : 0;
    /*
    assert(type.nullable);

    auto & codeGen = getThreadLocalCodeGen();

    llvm::Type * valueTy = getLLVMType();

    llvm::Value * innerValuePtr = codeGen->CreateStructGEP(valueTy, ptr, 0);
    _sqlValue->store(innerValuePtr);

    llvm::Value * nullIndicatorPtr = codeGen->CreateStructGEP(valueTy, ptr, 1);
    codeGen->CreateStore(_nullIndicator.llvmValue, nullIndicatorPtr);
    */
}

hash_t NullableValue::hash() const
{
    constexpr hash_t nullHash = 0x4d1138f9dd82ae2f; // arbitrary random number
    if (!isNull()) {
        return _sqlValue->hash();
    } else {
        return nullHash;
    }
}

bool NullableValue::isNull() const
{
    return _nullIndicator;
}

Value & NullableValue::getValue() const
{
    return *_sqlValue;
}
/*
void NullableValue::accept(ValueVisitor & visitor)
{
    visitor.visit(*this);
}
*/
bool NullableValue::equals(const Value & other) const
{
    if (!::Sql::equals(type, other.type, ::Sql::SqlTypeEqualsMode::WithoutNullable)) {
        return false;
    }

    // IS NOT DISTINCT FROM
    const bool otherIsNull = isNullable(other) && static_cast<const NullableValue &>(other).isNull();
    if (isNull() || otherIsNull) {
        return isNull() && otherIsNull;
    }
    return _sqlValue->equals(getAssociatedValue(other));
}

bool NullableValue::compare(const Value & other, ComparisonMode mode) const
{
    if (!::Sql::equals(type, other.type, ::Sql::SqlTypeEqualsMode::WithoutNullable)) {
        return false;
    }

    if (isNull()) {
        return false;
    } else {
        return _sqlValue->compare(other, mode);
    }
}


//-----------------------------------------------------------------------------
// utils

bool isNullable(const Value & value)
{
    return (dynamic_cast<const NullableValue *>(&value) != nullptr);
}

const Value & getAssociatedValue(const Value & value)
{
    if (const NullableValue * nv = dynamic_cast<const NullableValue *>(&value)) {
        return nv->getValue();
    } else {
        return value;
    }
}


value_op_t toColumnValue(value_op_t value, SqlType type)
{
    if (!type.nullable || isNullable(*value)) return value;
    return NullableValue::create(std::move(value), false);
}

value_op_t castColumnValue(const std::string & str, SqlType type)
{
    if (str == nullLiteral) {
        if (!type.nullable) throw std::invalid_argument("null value in a column declared NOT NULL");
        return NullableValue::getNull(type);
    }
    return toColumnValue(Value::castString(str, toNotNullableTy(type)), type);
}

std::string toString(const Value & sqlValue)
{
    if (isNullable(sqlValue)) {
        auto & nullable = static_cast<const NullableValue &>(sqlValue);
        return nullable.isNull() ? "NULL" : toString(nullable.getValue());
    }

    switch (sqlValue.type.typeID) {
        case SqlType::TypeID::UnknownID:
            return "null";
        case SqlType::TypeID::BoolID: {
            const Bool * b = dynamic_cast<const Bool *>(&sqlValue);
            return std::to_string(b->value);
        }
        case SqlType::TypeID::IntegerID: {
            const Integer * i = dynamic_cast<const Integer *>(&sqlValue);
            return std::to_string(i->value);
        }
        case SqlType::TypeID::LongIntegerID: // held by a Numeric
            return std::to_string(dynamic_cast<const Numeric *>(&sqlValue)->value);
        case SqlType::TypeID::VarcharID:
        case SqlType::TypeID::CharID: {
            // result tuples carry both as a Varchar that points to the characters
            const Varchar * v = dynamic_cast<const Varchar *>(&sqlValue);
            if (v == nullptr) throw NotImplementedException();
            std::string s(v->value, v->len);
            s.erase(std::find(s.begin(), s.end(), '\0'), s.end());
            return s;
        }
        case SqlType::TypeID::NumericID: {
            std::string ret = "";
            long v = dynamic_cast<const Numeric *>(&sqlValue)->value;
            if (v<0) { ret += "-"; v=-v; }
            long sep=1;
            for (unsigned index=0,limit=sqlValue.type.numericLayout.precision;index<limit;index++)
                sep*=10;
            ret+=std::to_string(v/sep);
            if(sqlValue.type.numericLayout.precision){
                v=v%sep;
                ret+=".";
                for (unsigned index=1,limit=sqlValue.type.numericLayout.precision;index++<limit && sep>(10*v); sep/=10)
                    ret+="0";
                ret+=std::to_string(v);
            }
            return ret;
        }
        case SqlType::TypeID::DateID:
            return formatDate(dynamic_cast<const Date *>(&sqlValue)->value);
        case SqlType::TypeID::TimestampID:
            return formatTimestamp(dynamic_cast<const Timestamp *>(&sqlValue)->value);
        case SqlType::TypeID::TextID: {
            const Text * text = dynamic_cast<const Text *>(&sqlValue);
            return std::string(text->getView());
        }
        case SqlType::TypeID::DoubleID:
            return formatDouble(dynamic_cast<const Double &>(sqlValue).value);
        case SqlType::TypeID::ArrayID:
            return dynamic_cast<const Array &>(sqlValue).toString();
        default:
            throw NotImplementedException();
    }
}

bool hasOrder(SqlType type)
{
    using T = SqlType::TypeID;
    switch (type.typeID) {
        case T::BoolID: case T::IntegerID: case T::LongIntegerID: case T::NumericID: case T::DateID:
        case T::TimestampID: case T::DoubleID: case T::TextID: case T::CharID: case T::VarcharID:
            return true;
        default:
            return false;
    }
}

int compareValues(const Value & a, const Value & b)
{
    using T = SqlType::TypeID;
    auto three = [](auto x, auto y) { return x < y ? -1 : (y < x ? 1 : 0); };
    switch (a.type.typeID) {
        case T::BoolID: return three(static_cast<const Bool &>(a).value, static_cast<const Bool &>(b).value);
        case T::IntegerID: return three(static_cast<const Integer &>(a).value, static_cast<const Integer &>(b).value);
        case T::LongIntegerID: // held by a Numeric
        case T::NumericID: return three(static_cast<const Numeric &>(a).value, static_cast<const Numeric &>(b).value);
        case T::DateID: return three(static_cast<const Date &>(a).value, static_cast<const Date &>(b).value);
        case T::TimestampID: return three(static_cast<const Timestamp &>(a).value, static_cast<const Timestamp &>(b).value);
        case T::DoubleID: {
            const double x = static_cast<const Double &>(a).value, y = static_cast<const Double &>(b).value;
            if (std::isnan(x) || std::isnan(y)) return three(std::isnan(x), std::isnan(y));
            return three(x, y);
        }
        case T::TextID: {
            const int c = static_cast<const Text &>(a).getView().compare(static_cast<const Text &>(b).getView());
            return three(c, 0);
        }
        case T::CharID: case T::VarcharID: {
            auto view = [](const Value & v) {
                auto & varchar = static_cast<const Varchar &>(v);
                std::string_view s(varchar.value, varchar.len);
                s = s.substr(0, std::min(s.size(), s.find('\0')));
                if (v.type.typeID == T::CharID) s = s.substr(0, s.find_last_not_of(' ') + 1);
                return s;
            };
            return three(view(a).compare(view(b)), 0);
        }
        default:
            throw std::invalid_argument("values of type " + ::Sql::getName(a.type) + " have no order");
    }
}

} // end namespace Sql
} // end namespace Native
