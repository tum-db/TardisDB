#include "native/sql/SqlValues.hpp"

#include <functional>
#include <limits>

#include "foundations/exceptions.hpp"
#include "foundations/LegacyTypes.hpp"
#include "foundations/utils.hpp"
#include "foundations/StringPool.hpp"
#include "sql/SqlType.hpp"

using Sql::SqlType;

namespace Native {
namespace Sql {

//-----------------------------------------------------------------------------
// common functions

// TODO move
template<class T>
static hash_t hashInteger(T value) {
    hash_t x = value;
    hash_t r = 88172645463325252ul;
    r = r ^ x;
    r = r ^ (r << 13ul);
    r = r ^ (r >> 7ul);
    r = r ^ (r << 17ul);
    return r;
}

// TODO move
static hash_t hashByteArray(const uint8_t * bytes, size_t len) {
    // TODO vectorized hash function
    hash_t h = 0;
    for (size_t i = 0; i < len; ++i) {
        h = (h << 5) | (h >> 27);
        h ^= static_cast<hash_t>(bytes[i]);
    }
    return h;
}

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
//        case SqlType::TypeID::VarcharID:
//            return Varchar::castString(str, type);
//        case SqlType::TypeID::CharID:
//            return Char::castString(str, type);
        case SqlType::TypeID::NumericID:
            return Numeric::castString(str, type);
        case SqlType::TypeID::DateID:
            return Date::castString(str);
        case SqlType::TypeID::TimestampID:
            return Timestamp::castString(str);
        default:
            unreachable();
    }
}

value_op_t Value::load(void * ptr, SqlType type)
{
    if (type.nullable) {
//        return NullableValue::load(ptr, type);
        throw NotImplementedException();
    }

    switch (type.typeID) {
        case SqlType::TypeID::UnknownID:
            return UnknownValue::create();
        case SqlType::TypeID::BoolID:
            throw NotImplementedException(); // TODO
        case SqlType::TypeID::IntegerID:
            return Integer::load(ptr);
//        case SqlType::TypeID::VarcharID:
//            return Varchar::load(ptr, type);
//        case SqlType::TypeID::CharID:
//            return Char::load(ptr, type);
        case SqlType::TypeID::NumericID:
            return Numeric::load(ptr, type);
        case SqlType::TypeID::DateID:
            return Date::load(ptr);
        case SqlType::TypeID::TimestampID:
            return Timestamp::load(ptr);
        default:
            unreachable();
    }
}

size_t Value::getSize() const
{
    return getValueSize(type);
}

//-----------------------------------------------------------------------------
// Integer

Integer::Integer(value_type constantValue) :
    Value(::Sql::getIntegerTy()),
    value(value)
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

value_op_t Integer::load(void * ptr)
{
    value_type value = *static_cast<value_type *>(ptr);
    value_op_t sqlValue( new Integer(value) );
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

Numeric::Numeric(SqlType type, llvm::Value * value) :
        Value(type)
{
    this->_llvmValue = value;
    assert(type.typeID == SqlType::TypeID::NumericID && !type.nullable);
}

Numeric::Numeric(SqlType type, value_type constantValue) :
        Value(type)
{
    _llvmValue = cg_value_type(constantValue);
}

value_op_t Numeric::clone() const
{
    Value * cloned = new Numeric(type, _llvmValue);
    return value_op_t(cloned);
}

value_op_t Numeric::castString(const std::string & str, SqlType type)
{
    int64_t raw = castStringToNumericValue(
            str.c_str(),
            static_cast<uint32_t>(str.size()),
            type.numericLayout.length,
            type.numericLayout.precision
    );
    llvm::Value * value = cg_value_type(raw);
    value_op_t sqlValue( new Numeric(type, value) );
    return sqlValue;
}

value_op_t Numeric::castString(cg_ptr8_t str, cg_size_t length, SqlType type)
{
    cg_i64_t raw = genCastStringToNumericValueCall(
            str, length,
            cg_unsigned_t(type.numericLayout.precision),
            cg_unsigned_t(type.numericLayout.precision)
    );
    value_op_t sqlValue( new Numeric(type, raw) );
    return sqlValue;
}

value_op_t Numeric::fromRawValues(const std::vector<llvm::Value *> & values, SqlType type)
{
    assert(!values.empty());
    return value_op_t(new Numeric(type, values.front()));
}

value_op_t Numeric::load(llvm::Value * ptr, SqlType type)
{
    auto & codeGen = getThreadLocalCodeGen();
    llvm::Type * numericTy = getPointeeType(ptr);
    assert(numericTy == cg_value_type::getType());

    llvm::Value * value = codeGen->CreateLoad(numericTy, ptr);
    value_op_t sqlValue( new Numeric(type, value) );
    return sqlValue;
}

void Numeric::store(llvm::Value * ptr) const
{
    auto & codeGen = getThreadLocalCodeGen();
    codeGen->CreateStore(_llvmValue, ptr);
}

cg_hash_t Numeric::hash() const
{
    // can be used for all integral types
    return hashInteger(value);
}

void Numeric::accept(ValueVisitor & visitor)
{
    visitor.visit(*this);
}

cg_bool_t Numeric::equals(const Value & other) const
{
    if (!Sql::equals(type, other.type, SqlTypeEqualsMode::WithoutNullable)) {
        return cg_bool_t(false);
    }

    if (isNullable(other)) {
        return other.equals(*this);
    }

    auto & codeGen = getThreadLocalCodeGen();
    return cg_bool_t( codeGen->CreateICmpEQ(_llvmValue, other.getLLVMValue()) );
}

//-----------------------------------------------------------------------------
// Bool

Bool::Bool(llvm::Value * value) :
        Value(getBoolTy())
{
    this->_llvmValue = value;
}

Bool::Bool(value_type constantValue) :
        Value(getBoolTy())
{
    auto & codeGen = getThreadLocalCodeGen();
    _llvmValue = codeGen->getInt1(constantValue);
}

value_op_t Bool::clone() const
{
    Value * cloned = new Bool(_llvmValue);
    return value_op_t(cloned);
}

value_op_t Bool::castString(const std::string & str)
{
    auto & codeGen = getThreadLocalCodeGen();

    llvm::Value * value;
    if (str == "true") { // TODO case insensitive
        value = codeGen->getInt1(true);
    } else if (str == "false") {
        value = codeGen->getInt1(false);
    } else {
        throw InvalidOperationException("invalid string representation of a boolean value");
    }

    value_op_t sqlValue( new Bool(value) );
    return sqlValue;
}

value_op_t Bool::castString(cg_ptr8_t str, cg_size_t length)
{
    throw NotImplementedException();
}

value_op_t Bool::fromRawValues(const std::vector<llvm::Value *> & values)
{
    assert(!values.empty());
    return value_op_t(new Bool(values.front()));
}

value_op_t Bool::load(llvm::Value * ptr)
{
    auto & codeGen = getThreadLocalCodeGen();
    llvm::Type * boolTy = getPointeeType(ptr);
    assert(boolTy == codeGen->getInt1Ty());

    llvm::Value * value = codeGen->CreateLoad(boolTy, ptr);
    value_op_t sqlValue( new Bool(value) );
    return sqlValue;
}

void Bool::store(llvm::Value * ptr) const
{
    auto & codeGen = getThreadLocalCodeGen();
    codeGen->CreateStore(_llvmValue, ptr);
}

cg_hash_t Bool::hash() const
{
    return genIntHash(_llvmValue);
}

void Bool::accept(ValueVisitor & visitor)
{
    visitor.visit(*this);
}

cg_bool_t Bool::equals(const Value & other) const
{
    if (!Sql::equals(type, other.type, SqlTypeEqualsMode::WithoutNullable)) {
        return cg_bool_t(false);
    }

    if (isNullable(other)) {
        return other.equals(*this);
    }

    auto & codeGen = getThreadLocalCodeGen();
    return cg_bool_t( codeGen->CreateICmpEQ(_llvmValue, other.getLLVMValue()) );
}

#if 0
//-----------------------------------------------------------------------------
// BasicString

BasicString::BasicString(SqlType type, llvm::Value * value, llvm::Value * length) :
        Value(type)
{
    this->_llvmValue = value;
    this->_length = length;
}

std::vector<llvm::Value *> BasicString::getRawValues() const
{
    return { _llvmValue, _length };
}

static inline unsigned lengthIndicatorSize(size_t length)
{
    return (length < 256) ? 8 : (length < 65536) ? 16 : 32;
}

llvm::Value * BasicString::loadStringLength(llvm::Value * ptr, SqlType type)
{
    auto & codeGen = getThreadLocalCodeGen();

    unsigned size = lengthIndicatorSize(type.length);

    // depending on the max size the length indicator has different sizes
    switch (size) {
        case 8:
            // load i8
            return codeGen->CreateLoad(codeGen->getInt8Ty(), ptr);
        case 16:
            // load i16
            return codeGen->CreateLoad(codeGen->getInt16Ty(), ptr);
        case 32:
            // load i32
            return codeGen->CreateLoad(codeGen->getInt32Ty(), ptr);
        default:
            unreachable();
    }
}

cg_bool_t BasicString::equalBuffers(const BasicString & str1, const BasicString & str2)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & funcGen = codeGen.getCurrentFunctionGen();

    // two strings can only be equal if they are of equal length
    cg_bool_t lenEQ( codeGen->CreateICmpEQ(str1.getLength(), str2.getLength()) );

    // now check if the strings actually match
    IfGen check( funcGen, lenEQ, {{"eq", cg_bool_t(false)}} );
    {
        cg_size_t len(str1.getLength());

        cg_voidptr_t s1(str1.getLLVMValue());
        cg_voidptr_t s2(str2.getLLVMValue());
        cg_int_t diff = Functions::genMemcmpCall(s1, s2, len);

        check.setVar( 0, diff == cg_int_t(0) );
    }
    check.EndIf();

    return cg_bool_t( check.getResult(0) );
}

//-----------------------------------------------------------------------------
// Char

Char::Char(SqlType type, llvm::Value * value, llvm::Value * length) :
        BasicString(type, value, length)
{
    assert(type.typeID == SqlType::TypeID::CharID && !type.nullable);
}

value_op_t Char::clone() const
{
    return value_op_t( new Char(type, _llvmValue, _length) );
}

value_op_t Char::castString(const std::string & str, SqlType type)
{
    auto & codeGen = getThreadLocalCodeGen();
    llvm::Value * strValue, * length;

    // only Chars with a length of 1 need special treatment
    if (type.length == 1) {
        assert(str.size() == 1);
        strValue = codeGen->getInt8(static_cast<uint8_t>(str[0]));
        length = codeGen->getInt8(1);
    } else {
        unsigned len = static_cast<unsigned>(str.size());
        uint32_t maxLen = type.length;

        // str should fit into the provided Varchar type
        if (len > maxLen) {
            throw std::runtime_error("type mismatch");
        }

        unsigned size = lengthIndicatorSize(type.length);
        length = codeGen->getIntN(size, len);
        strValue = codeGen->CreateGlobalString(str);
    }

    return value_op_t( new Char(type, strValue, length) );
}

value_op_t Char::castString(cg_ptr8_t str, cg_size_t length, SqlType type)
{
    auto & codeGen = getThreadLocalCodeGen();
    llvm::Value * strValue, * truncLength;

    // only Chars with a length of 1 need special treatment
    if (type.length == 1) {
        truncLength = codeGen->getInt8(1);
        strValue = codeGen->CreateLoad(codeGen->getInt8Ty(), str);
    } else {
        unsigned size = lengthIndicatorSize(type.length);
        truncLength = codeGen->CreateTrunc(length, codeGen->getIntNTy(size));
        strValue = str;
    }

    return value_op_t( new Char(type, strValue, truncLength) );
}

value_op_t Char::fromRawValues(const std::vector<llvm::Value *> & values, SqlType type)
{
    assert(values.size() >= 2);
    return value_op_t(new Char(type, values[0], values[1]));
}

value_op_t Char::load(llvm::Value * ptr, SqlType type)
{
    auto & codeGen = getThreadLocalCodeGen();
    llvm::Value * strValue, * length;
    llvm::Type * charTy = getPointeeType(ptr);

    // only Chars with a length of 1 need special treatment
    if (type.length == 1) {
        strValue = codeGen->CreateLoad(charTy, ptr);
        length = codeGen->getInt8(1);
    } else {
        strValue = codeGen->CreateStructGEP(charTy, ptr, 1);
        length = loadStringLength(ptr, type);
    }

    return value_op_t( new Char(type, strValue, length) );
}

void Char::store(llvm::Value * ptr) const
{
    auto & codeGen = getThreadLocalCodeGen();

    // only Chars with a length of 1 need special treatment
    if (type.length == 1) {
        codeGen->CreateStore(_llvmValue, ptr);
    } else {
        llvm::Type * valueTy = getLLVMType();

        // store the Char's length
        codeGen->CreateStore(_length,  ptr);

        // store the Char's data
        llvm::Value * strPtr = codeGen->CreateStructGEP(valueTy, ptr, 1);
        /* CallInst * CreateMemCpy (
            Value *Dst, unsigned DstAlign,
            Value *Src, unsigned SrcAlign,
            Value *Size,
            bool isVolatile=false,
            MDNode *TBAATag=nullptr,
            MDNode *TBAAStructTag=nullptr,
            MDNode *ScopeTag=nullptr,
            MDNode *NoAliasTag=nullptr)
        */
        codeGen->CreateMemCpy(strPtr, 1, _llvmValue, 1, _length);
    }
}

cg_hash_t Char::hash() const
{
    // only Chars with a length of 1 need special treatment
    if (type.length == 1) {
        return genIntHash(_llvmValue);
    } else {
        auto & codeGen = getThreadLocalCodeGen();
        cg_ptr8_t strPtr(_llvmValue);
        return genStringHash(codeGen, strPtr, cg_size_t(_length));
    }
}

void Char::accept(ValueVisitor & visitor)
{
    visitor.visit(*this);
}

cg_bool_t Char::equals(const Value & other) const
{
    if (!Sql::equals(type, other.type, SqlTypeEqualsMode::WithoutNullable)) {
        return cg_bool_t(false);
    }

    if (isNullable(other)) {
        return other.equals(*this);
    }

    auto & codeGen = getThreadLocalCodeGen();
    if (type.length == 1) {
        return cg_bool_t( codeGen->CreateICmpEQ(_llvmValue, other.getLLVMValue()) );
    } else {
        return equalBuffers(*this, static_cast<const BasicString &>(other));
    }
}

cg_bool_t Char::compare(const Value & other, ComparisonMode mode) const
{
    throw NotImplementedException("Char::compare");
}

//-----------------------------------------------------------------------------
// Varchar

Varchar::Varchar(SqlType type, llvm::Value * value, llvm::Value * length) :
        BasicString(type, value, length)
{
    assert(type.typeID == SqlType::TypeID::VarcharID && !type.nullable);
}

value_op_t Varchar::clone() const
{
    return value_op_t( new Varchar(type, _llvmValue, _length) );
}

value_op_t Varchar::castString(const std::string & str, SqlType type)
{
    auto & codeGen = getThreadLocalCodeGen();

    unsigned len = static_cast<unsigned>(str.size());
    uint32_t maxLen = type.length;

    // str should fit into the provided Varchar type
    if (len > maxLen) {
        throw std::runtime_error("type mismatch");
    }

    unsigned size = lengthIndicatorSize(type.length);
    llvm::Value * length = codeGen->getIntN(size, len);
    llvm::Value * strValue = codeGen->CreateGlobalString(str);

    return value_op_t( new Varchar(type, strValue, length) );
}

value_op_t Varchar::castString(cg_ptr8_t str, cg_size_t length, SqlType type)
{
    auto & codeGen = getThreadLocalCodeGen();

    unsigned size = lengthIndicatorSize(type.length);
    llvm::Value * truncLength = codeGen->CreateTrunc(length, codeGen->getIntNTy(size));
    return value_op_t( new Varchar(type, str, truncLength) );
}

value_op_t Varchar::fromRawValues(const std::vector<llvm::Value *> & values, SqlType type)
{
    assert(values.size() >= 2);
    return value_op_t(new Varchar(type, values[0], values[1]));
}

value_op_t Varchar::load(llvm::Value * ptr, SqlType type)
{
    auto & codeGen = getThreadLocalCodeGen();

    llvm::Type * varcharTy = getPointeeType(ptr);
    llvm::Value * strPtr = codeGen->CreateStructGEP(varcharTy, ptr, 1);
    llvm::Value * length = loadStringLength(ptr, type);
    return value_op_t( new Varchar(type, strPtr, length) );
}

void Varchar::store(llvm::Value * ptr) const
{
    auto & codeGen = getThreadLocalCodeGen();
    llvm::Type * valueTy = getLLVMType();

    // store the Varchar's length
    codeGen->CreateStore(_length,  ptr);

    // store the Varchar's data
    llvm::Value * strPtr = codeGen->CreateStructGEP(valueTy, ptr, 1);
//    Functions::genPrintfCall("store Varchar at: %p\n", strPtr);
    /* CallInst * CreateMemCpy (
        Value *Dst, unsigned DstAlign,
        Value *Src, unsigned SrcAlign,
        Value *Size,
        bool isVolatile=false,
        MDNode *TBAATag=nullptr,
        MDNode *TBAAStructTag=nullptr,
        MDNode *ScopeTag=nullptr,
        MDNode *NoAliasTag=nullptr)
    */
    codeGen->CreateMemCpy(strPtr, 1, _llvmValue, 1, _length);
}

cg_hash_t Varchar::hash() const
{
    auto & codeGen = getThreadLocalCodeGen();

    cg_ptr8_t strPtr(_llvmValue);
    return genStringHash(codeGen, strPtr, cg_size_t(_length));
}

void Varchar::accept(ValueVisitor & visitor)
{
    visitor.visit(*this);
}

cg_bool_t Varchar::equals(const Value & other) const
{
    if (!Sql::equals(type, other.type, SqlTypeEqualsMode::WithoutNullable)) {
        return cg_bool_t(false);
    }

    if (isNullable(other)) {
        return other.equals(*this);
    }

    return equalBuffers(*this, static_cast<const BasicString &>(other));
}

cg_bool_t Varchar::compare(const Value & other, ComparisonMode mode) const
{
    throw NotImplementedException("Varchar::compare");
}
#endif

//-----------------------------------------------------------------------------
// Date

Date::Date(llvm::Value * value) :
        Value(getDateTy())
{
    this->_llvmValue = value;
}

Date::Date(value_type constantValue) :
        Value(getDateTy())
{
    _llvmValue = cg_value_type(constantValue);
}

value_op_t Date::clone() const
{
    Value * cloned = new Date(_llvmValue);
    return value_op_t(cloned);
}

value_op_t Date::castString(const std::string & str)
{
    value_type raw = castStringToDateValue(str.c_str(), static_cast<uint32_t>(str.length()));
    llvm::Value * value = cg_value_type(raw);
    value_op_t sqlValue( new Date(value) );
    return sqlValue;
}

value_op_t Date::castString(cg_ptr8_t str, cg_size_t length)
{
    cg_value_type raw = genCastStringToDateValueCall(str, length);
    value_op_t sqlValue( new Date(raw) );
    return sqlValue;
}

value_op_t Date::fromRawValues(const std::vector<llvm::Value *> & values)
{
    assert(!values.empty());
    return value_op_t(new Date(values.front()));
}

value_op_t Date::load(llvm::Value * ptr)
{
    auto & codeGen = getThreadLocalCodeGen();
    llvm::Type * dateTy = getPointeeType(ptr);
    assert(dateTy == cg_value_type::getType());

    llvm::Value * value = codeGen->CreateLoad(dateTy, ptr);
    value_op_t sqlValue( new Date(value) );
    return sqlValue;
}

void Date::store(llvm::Value * ptr) const
{
    auto & codeGen = getThreadLocalCodeGen();
    codeGen->CreateStore(_llvmValue, ptr);
}

cg_hash_t Date::hash() const
{
    // can be used for all integral types
    return genIntHash(_llvmValue);
}

void Date::accept(ValueVisitor & visitor)
{
    visitor.visit(*this);
}

cg_bool_t Date::equals(const Value & other) const
{
    if (!Sql::equals(type, other.type, SqlTypeEqualsMode::WithoutNullable)) {
        return cg_bool_t(false);
    }

    if (isNullable(other)) {
        return other.equals(*this);
    }

    auto & codeGen = getThreadLocalCodeGen();
    return cg_bool_t( codeGen->CreateICmpEQ(_llvmValue, other.getLLVMValue()) );
}

//-----------------------------------------------------------------------------
// Timestamp

Timestamp::Timestamp(llvm::Value * value) :
        Value(getTimestampTy())
{
    this->_llvmValue = value;
}

Timestamp::Timestamp(value_type constantValue) :
        Value(getTimestampTy())
{
    _llvmValue = cg_value_type(constantValue);
}

value_op_t Timestamp::clone() const
{
    Value * cloned = new Timestamp(_llvmValue);
    return value_op_t(cloned);
}

value_op_t Timestamp::castString(const std::string & str)
{
    value_type raw = castStringToTimestampValue(str.c_str(), static_cast<uint32_t>(str.length()));
    llvm::Value * value = cg_value_type(raw);
    value_op_t sqlValue( new Timestamp(value) );
    return sqlValue;
}

value_op_t Timestamp::castString(cg_ptr8_t str, cg_size_t length)
{
    cg_value_type raw = genCastStringToTimestampValueCall(str, length);
    value_op_t sqlValue( new Timestamp(raw) );
    return sqlValue;
}

value_op_t Timestamp::fromRawValues(const std::vector<llvm::Value *> & values)
{
    assert(!values.empty());
    return value_op_t(new Timestamp(values.front()));
}

value_op_t Timestamp::load(llvm::Value * ptr)
{
    auto & codeGen = getThreadLocalCodeGen();
    llvm::Type * timestampTy = getPointeeType(ptr);
    assert(timestampTy == cg_value_type::getType());

    llvm::Value * value = codeGen->CreateLoad(timestampTy, ptr);
    value_op_t sqlValue( new Timestamp(value) );
    return sqlValue;
}

void Timestamp::store(llvm::Value * ptr) const
{
    auto & codeGen = getThreadLocalCodeGen();
    codeGen->CreateStore(_llvmValue, ptr);
}

cg_hash_t Timestamp::hash() const
{
    // can be used for all integral types
    return genIntHash(_llvmValue);
}

void Timestamp::accept(ValueVisitor & visitor)
{
    visitor.visit(*this);
}

cg_bool_t Timestamp::equals(const Value & other) const
{
    if (!Sql::equals(type, other.type, SqlTypeEqualsMode::WithoutNullable)) {
        return cg_bool_t(false);
    }

    if (isNullable(other)) {
        return other.equals(*this);
    }

    auto & codeGen = getThreadLocalCodeGen();
    return cg_bool_t( codeGen->CreateICmpEQ(_llvmValue, other.getLLVMValue()) );
}

//-----------------------------------------------------------------------------
// Text

Text::Text(bool inplace, value_type raw) :
    Value(::Sql::getTextTy(inplace)),
    value(raw)
{ }

Text::Text(const uint8_t * begin, const uint8_t * end) :
    Value(::Sql::getTextTy(false))
{
    value[0] = reinterpret_cast<intptr_t>(begin);
    value[1] = reinterpret_cast<intptr_t>(end);
}

Text::Text(uint8_t len, const uint8_t * bytes) :
    Value(::Sql::getTextTy(true))
{
    assert(len <= 15);
    uint8_t * data = reinterpret_cast<uint8_t *>(value.data());
    data[0] = len;
    std::memcpy(&data[1], bytes, len);
}

value_op_t Text::clone() const
{
    Value * cloned = new Text(type.inplace, value);
    return value_op_t(cloned);
}

value_op_t Text::castString(const std::string & str)
{
    const uint8_t * bytes = reinterpret_cast<const uint8_t *>(str.c_str());
    size_t len = str.size();
    if (len > 15) {
        std::unique_ptr<uint8_t[]> data(new uint8_t[len]);
        std::memcpy(data.get(), bytes, len);
//        uint8_t * begin = data.get();
//        uint8_t * end = begin+len;
//        auto sqlStr = std::make_unique<sql_string_t>(len, std::move(data));
        auto sqlStr = sql_string_t(len, std::move(data));
        auto storedStr = StringPool::instance().put(std::move(sqlStr));
        uint8_t * begin = storedStr.second.get();
        uint8_t * end = begin+len;
        value_op_t sqlValue( new Text(begin, end) );
    } else {
        value_op_t sqlValue( new Text(static_cast<uint8_t>(len), bytes) );
        return sqlValue;
    }
}

value_op_t Text::load(void * ptr)
{
    intptr_t * raw = static_cast<intptr_t *>(ptr);

    // retrieve the leftmost bit
    bool inplace = (0 == (raw[0] >> 8*sizeof(intptr_t)-1));
    if (inplace) {
        uint8_t * data = static_cast<uint8_t *>(ptr);
        value_op_t sqlValue( new Text(data[0], data+1) );
        return sqlValue;
    } else {
        intptr_t begin = raw[0];
        // untag the leftmost bit
        begin ^= 1 << (8*sizeof(intptr_t)-1);
        uint8_t * beginPtr = reinterpret_cast<uint8_t *>(begin);
        uint8_t * endPtr = reinterpret_cast<uint8_t *>(raw[1]);
        value_op_t sqlValue( new Text(beginPtr, endPtr) );
        return sqlValue;
    }
}

void Text::store(void * ptr) const
{
    if (type.inplace) {
        std::memcpy(ptr, value.data(), value.size()*sizeof(intptr_t));
    } else {
        // tag the leftmost bit
        intptr_t begin = value[0];
        begin ^= 1 << (8*sizeof(intptr_t)-1);

        intptr_t * raw = static_cast<intptr_t *>(ptr);
        raw[0] = begin;
        raw[1] = value[1];
    }
}

hash_t Text::hash() const
{
    size_t len = length();
    if (type.inplace) {
        const uint8_t * data = reinterpret_cast<const uint8_t *>(value.data());
        return hashByteArray(&data[1], len);
    } else {
        const uint8_t * begin = reinterpret_cast<const uint8_t *>(value[0]);
        return hashByteArray(begin, len);
    }
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
throw NotImplementedException();
    // TODO
    if (type.inplace) {

    } else {

    }
}

bool Text::compare(const Value & other, ComparisonMode mode) const
{
    throw NotImplementedException();
}

size_t Text::length() const
{
    if (type.inplace) {
        const uint8_t * data = reinterpret_cast<const uint8_t *>(value.data());
        return data[0];
    } else {
        ptrdiff_t diff = value[1] - value[0];
        return diff;
    }
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

#ifndef USE_INTERNAL_NULL_INDICATOR

/// NullableValue (with external null indicator)
NullableValue::NullableValue(value_op_t sqlValue, cg_bool_t nullIndicator) :
        Value( Sql::toNullableTy( sqlValue->type ) ),
        _sqlValue(std::move(sqlValue)),
        _nullIndicator(nullIndicator)
{
//    Functions::genPrintfCall("NullableValue ctor nullable %d\n", _nullIndicator.llvmValue);
    assert(!_sqlValue->type.nullable && type.typeID != SqlType::TypeID::UnknownID);
    assert(dynamic_cast<NullableValue *>(_sqlValue.get()) == nullptr); // do not nest NullableValues

    this->_llvmValue = _sqlValue->getLLVMValue();
}

value_op_t NullableValue::getNull(SqlType type)
{
    SqlType notNullableType = toNotNullableTy(type);

    switch (type.typeID) {
        case SqlType::TypeID::BoolID: {
            value_op_t nullBool = Integer::castString("false");
            return create(std::move(nullBool), cg_bool_t(true));
        }
        case SqlType::TypeID::IntegerID: {
            value_op_t nullInteger = Integer::castString("0");
            return create(std::move(nullInteger), cg_bool_t(true));
        }
        case SqlType::TypeID::NumericID: {
            value_op_t nullNumeric = Numeric::castString("0", notNullableType);
            return create(std::move(nullNumeric), cg_bool_t(true));
        }
        case SqlType::TypeID::VarcharID: {
            value_op_t nullVarchar = Varchar::castString(std::string(type.length, ' '), notNullableType);
            return create(std::move(nullVarchar), cg_bool_t(true));
        }
        case SqlType::TypeID::CharID: {
            value_op_t nullChar = Char::castString(std::string(type.length, ' '), notNullableType);
            return create(std::move(nullChar), cg_bool_t(true));
        }
        case SqlType::TypeID::DateID: // TODO
        case SqlType::TypeID::TimestampID: // TODO
            throw NotImplementedException();
        default:
            unreachable();
    }
}

value_op_t NullableValue::create(value_op_t value, cg_bool_t nullIndicator)
{
    return value_op_t( new NullableValue(std::move(value), nullIndicator) );
}

value_op_t NullableValue::create(const Value & value, cg_bool_t nullIndicator)
{
    const Value & assoc = getAssociatedValue(value);
    return create(assoc.clone(), nullIndicator);
}

value_op_t NullableValue::load(llvm::Value * ptr, SqlType type)
{
    assert(type.nullable);

    auto & codeGen = getThreadLocalCodeGen();
    llvm::Type * valueTy = Sql::toLLVMTy(type);

    llvm::Value * innerValuePtr = codeGen->CreateStructGEP(valueTy, ptr, 0);
    SqlType notNullableType = Sql::toNotNullableTy(type);
    value_op_t sqlValue = Value::load(innerValuePtr, notNullableType);

    llvm::Value * nullIndicatorPtr = codeGen->CreateStructGEP(valueTy, ptr, 1);
    cg_bool_t nullIndicator( codeGen->CreateLoad(cg_bool_t::getType(), nullIndicatorPtr) );

    return NullableValue::create(std::move(sqlValue), cg_bool_t(nullIndicator));
}

value_op_t NullableValue::clone() const
{
    value_op_t cloned = _sqlValue->clone();
    return create(std::move(cloned), _nullIndicator);
}

std::vector<llvm::Value *> NullableValue::getRawValues() const
{
    auto values = _sqlValue->getRawValues();
    values.push_back(_nullIndicator.llvmValue);
    return values;
}

void NullableValue::store(llvm::Value * ptr) const
{
    assert(type.nullable);

    auto & codeGen = getThreadLocalCodeGen();

    llvm::Type * valueTy = getLLVMType();

    llvm::Value * innerValuePtr = codeGen->CreateStructGEP(valueTy, ptr, 0);
    _sqlValue->store(innerValuePtr);

    llvm::Value * nullIndicatorPtr = codeGen->CreateStructGEP(valueTy, ptr, 1);
    codeGen->CreateStore(_nullIndicator.llvmValue, nullIndicatorPtr);
}

cg_hash_t NullableValue::hash() const
{
    cg_hash_t nullHash = 0x4d1138f9dd82ae2f; // arbitrary random number

    PhiNode<llvm::Value> result(nullHash, "hash");
    IfGen nullCheck(!isNull());
    {
        result = _sqlValue->hash();
    }
    nullCheck.EndIf();

    return cg_hash_t( result.get() );
}

cg_bool_t NullableValue::isNull() const
{
    return _nullIndicator;
}

Value & NullableValue::getValue() const
{
    return *_sqlValue;
}

void NullableValue::accept(ValueVisitor & visitor)
{
    visitor.visit(*this);
}

cg_bool_t NullableValue::equals(const Value & other) const
{
    if (!Sql::equals(type, other.type, SqlTypeEqualsMode::WithoutNullable)) {
        return cg_bool_t(false);
    }

    PhiNode<llvm::Value> result(cg_bool_t(false), "equals");
    IfGen nullCheck(!isNull());
    {
        result = _sqlValue->equals(other);
    }
    nullCheck.EndIf();

    return cg_bool_t(result.get());
}

cg_bool_t NullableValue::compare(const Value & other, ComparisonMode mode) const
{
    if (!Sql::equals(type, other.type, SqlTypeEqualsMode::WithoutNullable)) {
        return cg_bool_t(false);
    }

    PhiNode<llvm::Value> result(cg_bool_t(false), "result");
    IfGen nullCheck(!isNull());
    {
        result = _sqlValue->compare(other, mode);
    }
    nullCheck.EndIf();

    return cg_bool_t(result.get());
}

#else

// null constants
static Integer::value_type integerNullIndicator = std::numeric_limits<typename Integer::value_type>::min();
static Numeric::value_type numericNullIndicator = std::numeric_limits<typename Numeric::value_type>::min();
// bool: nothing
// TODO: static Char::length_type charNullIndicator = numeric_limits<typename Char::length_type>::max();
// TODO: static Varchar::length_type varcharNullIndicator = numeric_limits<typename Varchar::length_type>::max();
static Date::value_type dateNullIndicator = std::numeric_limits<typename Date::value_type>::max();
static Timestamp::value_type timestampNullIndicator = std::numeric_limits<typename Timestamp::value_type>::max();

/// NullableValue (with internal null indicator)
NullableValue::NullableValue(value_op_t sqlValue) :
        Value( Sql::toNullableTy( sqlValue->type ) ),
        _sqlValue(std::move(sqlValue))
{
    assert(!_sqlValue->type.nullable && type.typeID != SqlType::TypeID::UnknownID);
    assert(dynamic_cast<NullableValue *>(_sqlValue.get()) == nullptr); // do not nest NullableValues

    this->_llvmValue = _sqlValue->getLLVMValue();
}

value_op_t NullableValue::getNull(SqlType type)
{
    SqlType notNullableType = toNotNullableTy(type);

    switch (type.typeID) {
        case SqlType::TypeID::BoolID: {
            throw InvalidOperationException("use an external null indicator instead");
        }
        case SqlType::TypeID::IntegerID: {
            value_op_t nullInteger( new Integer(integerNullIndicator) );
            return create(std::move(nullInteger));
        }
        case SqlType::TypeID::NumericID: {
            value_op_t nullNumeric( new Numeric(notNullableType, numericNullIndicator) );
            return create(std::move(nullNumeric));
        }
        case SqlType::TypeID::VarcharID: {
            throw NotImplementedException();
        }
        case SqlType::TypeID::CharID: {
            throw NotImplementedException();
        }
        case SqlType::TypeID::DateID: {
            value_op_t nullDate( new Date(dateNullIndicator) );
            return create(std::move(nullDate));
        }
        case SqlType::TypeID::TimestampID: {
            value_op_t nullTimestamp( new Timestamp(timestampNullIndicator) );
            return create(std::move(nullTimestamp));
        }
        default:
            unreachable();
    }
}

value_op_t NullableValue::create(value_op_t value)
{
    return value_op_t( new NullableValue(std::move(value)) );
}

value_op_t NullableValue::create(const Value & value)
{
    const Value & assoc = getAssociatedValue(value);
    return create(assoc.clone());
}

value_op_t NullableValue::load(llvm::Value * ptr, SqlType type)
{
    assert(type.nullable);

    SqlType notNullableType = Sql::toNotNullableTy(type);
    value_op_t sqlValue = Value::load(ptr, notNullableType);

    return NullableValue::create(std::move(sqlValue));
}

value_op_t NullableValue::clone() const
{
    value_op_t cloned = _sqlValue->clone();
    return create(std::move(cloned));
}

std::vector<llvm::Value *> NullableValue::getRawValues() const
{
    return _sqlValue->getRawValues();
}

void NullableValue::store(llvm::Value * ptr) const
{
    assert(type.nullable);
    _sqlValue->store(ptr);
}

cg_hash_t NullableValue::hash() const
{
    cg_hash_t nullHash = 0x4d1138f9dd82ae2f; // arbitrary random number

    PhiNode<llvm::Value> result(nullHash, "hash");
    IfGen nullCheck(!isNull());
    {
        result = _sqlValue->hash();
    }
    nullCheck.EndIf();

    return cg_hash_t( result.get() );
}

cg_bool_t NullableValue::isNull() const
{
    switch (type.typeID) {
        case SqlType::TypeID::BoolID: {
            throw InvalidOperationException("use an external null indicator instead");
        }
        case SqlType::TypeID::IntegerID: {
            Integer::cg_value_type a(integerNullIndicator);
            Integer::cg_value_type b(_sqlValue->getLLVMValue());
            return (a == b);
        }
        case SqlType::TypeID::NumericID: {
            Numeric::cg_value_type a(numericNullIndicator);
            Numeric::cg_value_type b(_sqlValue->getLLVMValue());
            return (a == b);
        }
        case SqlType::TypeID::VarcharID: {
            throw NotImplementedException();
        }
        case SqlType::TypeID::CharID: {
            throw NotImplementedException();
        }
        case SqlType::TypeID::DateID: {
            Date::cg_value_type a(dateNullIndicator);
            Date::cg_value_type b(_sqlValue->getLLVMValue());
            return (a == b);
        }
        case SqlType::TypeID::TimestampID: {
            Timestamp::cg_value_type a(timestampNullIndicator);
            Timestamp::cg_value_type b(_sqlValue->getLLVMValue());
            return (a == b);
        }
        default:
            unreachable();
    }
}

Value & NullableValue::getValue() const
{
    return *_sqlValue;
}

void NullableValue::accept(ValueVisitor & visitor)
{
    visitor.visit(*this);
}

cg_bool_t NullableValue::equals(const Value & other) const
{
    if (!Sql::equals(type, other.type, SqlTypeEqualsMode::WithoutNullable)) {
        return cg_bool_t(false);
    }

    PhiNode<llvm::Value> result(cg_bool_t(false), "equals");
    IfGen nullCheck(!isNull());
    {
        result = _sqlValue->equals(other);
    }
    nullCheck.EndIf();

    return cg_bool_t(result.get());
}

cg_bool_t NullableValue::compare(const Value & other, ComparisonMode mode) const
{
    if (!Sql::equals(type, other.type, SqlTypeEqualsMode::WithoutNullable)) {
        return cg_bool_t(false);
    }

    PhiNode<llvm::Value> result(cg_bool_t(false), "result");
    IfGen nullCheck(!isNull());
    {
        result = _sqlValue->compare(other, mode);
    }
    nullCheck.EndIf();

    return cg_bool_t(result.get());
}

#endif // USE_INTERNAL_NULL_INDICATOR

//-----------------------------------------------------------------------------
// utils

bool isNullable(const Value & value)
{
    return (dynamic_cast<const NullableValue *>(&value) != nullptr);
}

bool mayBeNull(const Value & value)
{
    if (value.type.typeID == SqlType::TypeID::UnknownID) {
        return true;
    }

    return (dynamic_cast<const NullableValue *>(&value) != nullptr);
}

bool isUnknown(const Value & value)
{
    return (value.type.typeID == SqlType::TypeID::UnknownID);
}

const Value & getAssociatedValue(const Value & value)
{
    if (const NullableValue * nv = dynamic_cast<const NullableValue *>(&value)) {
        return nv->getValue();
    } else {
        return value;
    }
}

bool isNumerical(const Value & value)
{
    using TypeID = SqlType::TypeID;
    auto typeID = value.type.typeID;
    return (typeID == TypeID::UnknownID || typeID == TypeID::IntegerID || typeID == TypeID::NumericID);
}

#ifdef USE_INTERNAL_NULL_INDICATOR
llvm::Value * getNullIndicator(SqlType type)
{
    switch (type.typeID) {
        case SqlType::TypeID::BoolID:
            return nullptr;
        case SqlType::TypeID::IntegerID:
            return Integer::cg_value_type(integerNullIndicator);
        case SqlType::TypeID::NumericID:
            return Numeric::cg_value_type(numericNullIndicator);
        case SqlType::TypeID::VarcharID: {
            throw NotImplementedException();
        }
        case SqlType::TypeID::CharID: {
            throw NotImplementedException();
        }
        case SqlType::TypeID::DateID:
            return Date::cg_value_type(dateNullIndicator);
        case SqlType::TypeID::TimestampID:
            return Timestamp::cg_value_type(timestampNullIndicator);
        default:
            unreachable();
    }
}
#endif // USE_INTERNAL_NULL_INDICATOR

} // end namespace Sql
} // end namespace Native
