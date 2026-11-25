#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include "native/sql/Array.hpp"
#include "sql/SqlType.hpp"
#include "utils/hashing.hpp"

namespace Native {
namespace Sql {

class Value;
class ValueVisitor;

using value_op_t = std::unique_ptr<Value>;
using ::Sql::SqlType;

enum class ComparisonMode { less, leq, eq, geq, gtr };

//-----------------------------------------------------------------------------
// Value

class Value {
public:
    const SqlType type;

    Value(const Value & other) = delete;
    Value & operator =(const Value & rhs) = delete;

    virtual ~Value();

    /// \brief Use the given string to construct a SQL value with the given type
    static value_op_t castString(const std::string & str, SqlType type);

    /// \brief Construct a SQL value out of the data at the address given by ptr
    static value_op_t load(const void * ptr, SqlType type);

    virtual size_t getSize() const;

    virtual value_op_t clone() const = 0;

    virtual void store(void * ptr) const = 0;

    virtual hash_t hash() const = 0;

//    virtual void accept(ValueVisitor & visitor) = 0;

    /// \note Only Values with matching types are considered to be comparable.
    /// Therefore a type cast may be necessary prior to calling equals().
    virtual bool equals(const Value & other) const = 0;

    virtual bool compare(const Value & other, ComparisonMode mode) const = 0;

protected:
    Value(SqlType type);
};

//-----------------------------------------------------------------------------
// Integer

class Integer : public Value {
public:
    using value_type = int32_t;
    value_type value;

    explicit Integer();

    explicit Integer(SqlType type);

    explicit Integer(const void * src);

    explicit Integer(value_type constantValue);

    static value_op_t castString(const std::string & str);

    static value_op_t load(const void * ptr);

    value_op_t clone() const override;

    void store(void * ptr) const override;

    hash_t hash() const override;

//    void accept(ValueVisitor & visitor) override;

    bool equals(const Value & other) const override;

    bool compare(const Value & other, ComparisonMode mode) const override;
};

//-----------------------------------------------------------------------------
// Value

class Numeric : public Value {
public:
    using value_type = int64_t;
    value_type value;

    explicit Numeric();

    explicit Numeric(SqlType type);

    explicit Numeric(const void * src, SqlType type);

    explicit Numeric(SqlType type, value_type constantValue);

    static value_op_t castString(const std::string & str, SqlType type);

    static value_op_t load(const void * ptr, SqlType type);

    value_op_t clone() const override;

    void store(void * ptr) const override;

    hash_t hash() const override;

//    void accept(ValueVisitor & visitor) override;

    bool equals(const Value & other) const override;

    bool compare(const Value & other, ComparisonMode mode) const override;
};

//-----------------------------------------------------------------------------
// Bool

class Bool : public Value {
public:
    using value_type = bool;
    value_type value;

    explicit Bool();

    explicit Bool(SqlType type);

    explicit Bool(const void * src);

    explicit Bool(value_type constantValue);

    static value_op_t castString(const std::string & str);

    static value_op_t load(const void * ptr);

    value_op_t clone() const override;

    void store(void * ptr) const override;

    hash_t hash() const override;

//    void accept(ValueVisitor & visitor) override;

    bool equals(const Value & other) const override;

    bool compare(const Value & other, ComparisonMode mode) const override;
};

//-----------------------------------------------------------------------------
// Text

class Text : public Value {
public:
    using value_type = std::array<uintptr_t, 2>;
    using string_ref_t = std::pair<size_t, const uint8_t *>;
    value_type value = {0,0};

    explicit Text();

    explicit Text(SqlType type);

    explicit Text(const void * src);

    static value_op_t castString(const std::string & str);

    static value_op_t load(const void * ptr);

    value_op_t clone() const override;

    void store(void * ptr) const override;

    hash_t hash() const override;

//    void accept(ValueVisitor & visitor) override;

    bool equals(const Value & other) const override;

    bool compare(const Value & other, ComparisonMode mode) const override;

    const uint8_t * begin() const;

    string_ref_t getString() const;

    // attention: sql text values may contain null bytes
    std::string_view getView() const;

    bool isInplace() const;

    size_t length() const;

private:
    Text(const uint8_t * beginPtr, const uint8_t * endPtr);
    Text(uint8_t len, const uint8_t * bytes);
};

//-----------------------------------------------------------------------------
// Double

/// DOUBLE PRECISION; as in PostgreSQL, NaN equals NaN and is greater than all other values
class Double : public Value {
public:
    using value_type = double;
    value_type value = 0;

    explicit Double(value_type constantValue = 0);

    static value_op_t castString(const std::string & str);

    static value_op_t load(const void * ptr);

    value_op_t clone() const override;

    void store(void * ptr) const override;

    hash_t hash() const override;

    bool equals(const Value & other) const override;

    bool compare(const Value & other, ComparisonMode mode) const override;
};

/// The text of PostgreSQL (shortest exact digits; exponent below 1e-4 and from 1e15 on)
std::string formatDouble(double value);

/// Parses a double as PostgreSQL does (also Infinity, -Infinity and NaN); throws std::invalid_argument
double parseDouble(const std::string & str);

/// The digits of a numeric of the given scale
std::string formatNumeric(int64_t raw, unsigned scale);

//-----------------------------------------------------------------------------
// Array

/// An array (see native/sql/Array.hpp): a handle of its representation. A value loaded from a computed
/// array (in the arena of a query) owns a copy, as it may outlive the query (a result tuple).
class Array : public Value {
public:
    Arrays::Handle value;

    explicit Array(SqlType type, Arrays::Handle handle);

    /// The text of PostgreSQL ('{1,2}'), stored in the string pool
    static value_op_t castString(const std::string & str, SqlType type);

    static value_op_t load(const void * ptr, SqlType type);

    value_op_t clone() const override;

    /// Stores a handle of the string pool
    void store(void * ptr) const override;

    hash_t hash() const override;

    bool equals(const Value & other) const override;

    bool compare(const Value & other, ComparisonMode mode) const override;

    std::string toString() const;

    /// Copies a computed array, so that the value outlives the query
    void ownTemporary();

private:
    std::shared_ptr<const std::string> _owned; // the copy of a computed array
};

//-----------------------------------------------------------------------------
// Varchar

        class Varchar : public Value {
        public:
            using value_type = const char *;
            value_type value;

            explicit Varchar(SqlType type) : Value(type) {
                value = nullptr;
            }

            explicit Varchar(const char * src,uint32_t length) : Value(::Sql::getVarcharTy(length,false)) {
                value = (char*)src;
                len = length;
            }

            static value_op_t castString(const std::string & str, SqlType type);

            static value_op_t load(const void * ptr, SqlType type);

            value_op_t clone() const override;

            void load(const void * ptr);

            void store(void * ptr) const override;

            hash_t hash() const override;

            bool equals(const Value & other) const override;

            bool compare(const Value & other, ComparisonMode mode) const override;

            const uint8_t * begin() const {
                return (const uint8_t *)value;
            }

            size_t length() const {
                return len;
            }

            uint32_t len;
            char single = 0; // the character of a char(1), which value then points to

        };

//-----------------------------------------------------------------------------
// Date

class Date : public Value {
public:
    using value_type = uint32_t;
    value_type value;

    explicit Date();

    explicit Date(SqlType type);

    explicit Date(const void * src);

    explicit Date(value_type constantValue);

    static value_op_t castString(const std::string & str);

    static value_op_t load(const void * ptr);

    value_op_t clone() const override;

    void store(void * ptr) const override;

    hash_t hash() const override;

//    void accept(ValueVisitor & visitor) override;

    bool equals(const Value & other) const override;

    bool compare(const Value & other, ComparisonMode mode) const override;
};

//-----------------------------------------------------------------------------
// Timestamp

class Timestamp : public Value {
public:
    using value_type = uint64_t;
    value_type value;

    explicit Timestamp();

    explicit Timestamp(SqlType type);

    explicit Timestamp(const void * src);

    explicit Timestamp(value_type constantValue);

    static value_op_t castString(const std::string & str);

    static value_op_t load(const void * ptr);

    value_op_t clone() const override;

    void store(void * ptr) const override;

    hash_t hash() const override;

//    void accept(ValueVisitor & visitor) override;

    bool equals(const Value & other) const override;

    bool compare(const Value & other, ComparisonMode mode) const override;
};

//-----------------------------------------------------------------------------
// UnknownValue

/// Represents a null value without any type associated
class UnknownValue : public Value {
public:
    static value_op_t create();

    void store(void * ptr) const override; // nop

    hash_t hash() const override; // arbitrary constant

    value_op_t clone() const override;

//    void accept(ValueVisitor & visitor) override;

    bool equals(const Value & other) const override;

    bool compare(const Value & other, ComparisonMode mode) const override;

protected:
    UnknownValue();
};

//-----------------------------------------------------------------------------
// NullableValue


/// NullableValue (with external null indicator): stored as the generated code stores it, the value of
/// the not nullable type followed by the null indicator (one byte, at the offset of the second member
/// of the struct of toLLVMTy()); the value of a NULL is zeroed
class NullableValue : public Value {
public:
    /// \returns A NULL of the type, whose value is the zeroed value of the not nullable type
    static value_op_t getNull(SqlType type);

    static value_op_t create(value_op_t value, bool nullIndicator);

    static value_op_t create(const Value & value, bool nullIndicator);

    static value_op_t load(const void * ptr, SqlType type);

    value_op_t clone() const override;

    void store(void * ptr) const override;

    hash_t hash() const override;

    bool isNull() const;

    Value & getValue() const;

    /// The null indicator, which the generated code sets when it translates a value into this one
    bool * getNullIndicatorPtr() { return &_nullIndicator; }

//    void accept(ValueVisitor & visitor) override;

    /// Not as SQL's =, but as IS NOT DISTINCT FROM, which MERGE and DIFF compare versions with: two
    /// NULLs are equal, a NULL and a value are not
    bool equals(const Value & other) const override;

    bool compare(const Value & other, ComparisonMode mode) const override;

protected:
    NullableValue(value_op_t sqlValue, bool nullIndicator);

    value_op_t _sqlValue;
    bool _nullIndicator;
};


//-----------------------------------------------------------------------------
// utilities

bool isNullable(const Value & value);

/// \returns \p value as a value of a column of \p type: wrapped into a NullableValue that is not NULL if
/// the type is nullable and the value is not a NullableValue already
value_op_t toColumnValue(value_op_t value, SqlType type);

/// The constant NULL among the values of a statement (INSERT, SET, WHERE, DEFAULT), which the parser
/// passes as text like the other constants; no literal spells it, as it starts with a zero byte
inline const std::string nullLiteral("\0NULL", 5);

/// \returns The value of \p str for a column of \p type (see toColumnValue()); nullLiteral is NULL,
/// which a column of a type that is not nullable does not take (std::invalid_argument)
value_op_t castColumnValue(const std::string & str, SqlType type);

/// \returns The Value associated with a NullableValue or the Value itself
const Value & getAssociatedValue(const Value & value);

std::string toString(const Value & sqlValue);

/// \returns The order of two values of one type that are not NULL, as ORDER BY and the merge join sort
/// them: < 0, 0 or > 0. Strings by their bytes (collation C; char(n) without trailing spaces), NaN after
/// all other doubles and equal to NaN, -0 equal to 0, as in PostgreSQL.
/// \throws std::invalid_argument for types without an order (arrays)
int compareValues(const Value & a, const Value & b);

/// \returns Whether compareValues() orders the values of \p type
bool hasOrder(SqlType type);

} // end namespace Sql
} // end namespace Native
