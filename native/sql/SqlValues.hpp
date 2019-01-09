#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>

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

#if 0
//-----------------------------------------------------------------------------
// BasicString

class BasicString : public Value {
public:
    std::vector<llvm::Value *> getRawValues() const override;

    llvm::Value * getLength() const
    {
        return _length;
    }

protected:
    BasicString(SqlType type, llvm::Value * value, llvm::Value * length);

    static llvm::Value * loadStringLength(llvm::Value * ptr, SqlType type);

    static cg_bool_t equalBuffers(const BasicString & str1, const BasicString & str2);

    static cg_int_t compareBuffers(const BasicString & str1, const BasicString & str2);

    llvm::Value * _length = nullptr;
};

//-----------------------------------------------------------------------------
// Char

class Char : public BasicString {
public:
    static value_op_t castString(const std::string & str, SqlType type);

    static value_op_t castString(cg_ptr8_t str, cg_size_t length, SqlType type);

    static value_op_t fromRawValues(const std::vector<llvm::Value *> & values, SqlType type);

    static value_op_t load(llvm::Value * ptr, SqlType type);

    value_op_t clone() const override;

    void store(llvm::Value * ptr) const override;

    cg_hash_t hash() const override;

    void accept(ValueVisitor & visitor) override;

    cg_bool_t equals(const Value & other) const override;

    cg_bool_t compare(const Value & other, ComparisonMode mode) const override;

protected:
    Char(SqlType type, llvm::Value * value, llvm::Value * length);
};

//-----------------------------------------------------------------------------
// Varchar

class Varchar : public BasicString {
public:
    static value_op_t castString(const std::string & str, SqlType type);

    static value_op_t castString(cg_ptr8_t str, cg_size_t length, SqlType type);

    static value_op_t fromRawValues(const std::vector<llvm::Value *> & values, SqlType type);

    static value_op_t load(llvm::Value * ptr, SqlType type);

    value_op_t clone() const override;

    void store(llvm::Value * ptr) const override;

    cg_hash_t hash() const override;

    void accept(ValueVisitor & visitor) override;

    cg_bool_t equals(const Value & other) const override;

    cg_bool_t compare(const Value & other, ComparisonMode mode) const override;

protected:
    Varchar(SqlType type, llvm::Value * value, llvm::Value * length);
};
#endif

//-----------------------------------------------------------------------------
// Text

class Text : public Value {
public:
    using value_type = std::array<uintptr_t, 2>;
    using string_ref_t = std::pair<size_t, const uint8_t *>;
    value_type value;

    explicit Text();

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
// Date

class Date : public Value {
public:
    using value_type = uint32_t;
    value_type value;

    explicit Date();

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

#ifndef USE_INTERNAL_NULL_INDICATOR

/// NullableValue (with external null indicator)
class NullableValue : public Value {
public:
    static value_op_t getNull(SqlType type);

    static value_op_t create(value_op_t value, bool nullIndicator);

    static value_op_t create(const Value & value, bool nullIndicator);

    static value_op_t load(const void * ptr, SqlType type);

    value_op_t clone() const override;

    void store(void * ptr) const override;

    hash_t hash() const override;

    bool isNull() const;

    Value & getValue() const;

//    void accept(ValueVisitor & visitor) override;

    bool equals(const Value & other) const override;

    bool compare(const Value & other, ComparisonMode mode) const override;

protected:
    NullableValue(value_op_t sqlValue, bool nullIndicator);

    value_op_t _sqlValue;
    bool _nullIndicator;
};

#else

/// NullableValue (with internal null indicator)
class NullableValue : public Value {
public:
    static value_op_t getNull(SqlType type);

    static value_op_t create(value_op_t value);

    static value_op_t create(const Value & value);

    static value_op_t load(llvm::Value * ptr, SqlType type);

    std::vector<llvm::Value *> getRawValues() const override;

    value_op_t clone() const override;

    void store(llvm::Value * ptr) const override;

    cg_hash_t hash() const override;

    cg_bool_t isNull() const;

    Value & getValue() const;

    void accept(ValueVisitor & visitor) override;

    cg_bool_t equals(const Value & other) const override;

    cg_bool_t compare(const Value & other, ComparisonMode mode) const override;

protected:
    NullableValue(value_op_t sqlValue);

    value_op_t _sqlValue;
};

#endif // USE_INTERNAL_NULL_INDICATOR

#if 0
//-----------------------------------------------------------------------------
// ValueVisitor

class ValueVisitor {
public:
    virtual void visit(Bool & value) = 0;
    virtual void visit(Char & value) = 0;
    virtual void visit(Date & value) = 0;
    virtual void visit(Integer & value) = 0;
    virtual void visit(Numeric & value) = 0;
    virtual void visit(Timestamp & value) = 0;
    virtual void visit(UnknownValue & value) = 0;
    virtual void visit(Varchar & value) = 0;
    virtual void visit(NullableValue & value) = 0;
};
#endif

//-----------------------------------------------------------------------------
// utilities

bool isNullable(const Value & value);

/// Checks whether the given Value is an instance of NullableValue or UnknownValue (null constant)
bool mayBeNull(const Value & value);

bool isUnknown(const Value & value);

/// \returns The Value associated with a NullableValue or the Value itself
const Value & getAssociatedValue(const Value & value);

bool isNumerical(const Value & value);

std::string toString(const Value & sqlValue);

} // end namespace Sql
} // end namespace Native
