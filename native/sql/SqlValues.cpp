#include "native/sql/SqlValues.hpp"

#include <functional>
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

value_op_t Value::load(const void * ptr, SqlType type)
{
    if (type.nullable) {
//        return NullableValue::load(ptr, type);
        throw NotImplementedException();
    }

    switch (type.typeID) {
        case SqlType::TypeID::UnknownID:
            return UnknownValue::create();
        case SqlType::TypeID::BoolID:
            return Bool::load(ptr);
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

Integer::Integer() :
    Value(::Sql::getIntegerTy()),
    value(0)
{ }

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
    bool value;
    if (str == "true") { // TODO case insensitive
        value = true;
    } else if (str == "false") {
        value = false;
    } else {
        throw InvalidOperationException("invalid string representation of a boolean value");
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

Date::Date() :
    Value(::Sql::getDateTy()),
    value(0)
{ }

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
// Text

Text::Text() :
    Value(::Sql::getTextTy())
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

size_t Text::length() const
{
    if (isInplace()) {
        const uint8_t * data = reinterpret_cast<const uint8_t *>(value.data());
        return data[0];
    } else {
        // remove tag
        uintptr_t beginValue = reinterpret_cast<uintptr_t>(begin());
        ptrdiff_t diff = value[1] - beginValue;
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
NullableValue::NullableValue(value_op_t sqlValue, bool nullIndicator) :
        Value( ::Sql::toNullableTy( sqlValue->type ) ),
        _sqlValue(std::move(sqlValue)),
        _nullIndicator(nullIndicator)
{
//    Functions::genPrintfCall("NullableValue ctor nullable %d\n", _nullIndicator.llvmValue);
    assert(!_sqlValue->type.nullable && type.typeID != SqlType::TypeID::UnknownID);
    assert(dynamic_cast<NullableValue *>(_sqlValue.get()) == nullptr); // do not nest NullableValues
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
        /*
        case SqlType::TypeID::VarcharID: {
            value_op_t nullVarchar = Varchar::castString(std::string(type.length, ' '), notNullableType);
            return create(std::move(nullVarchar), cg_bool_t(true));
        }
        case SqlType::TypeID::CharID: {
            value_op_t nullChar = Char::castString(std::string(type.length, ' '), notNullableType);
            return create(std::move(nullChar), cg_bool_t(true));
        }
        */
        case SqlType::TypeID::DateID: // TODO
        case SqlType::TypeID::TimestampID: // TODO
            throw NotImplementedException();
        default:
            unreachable();
    }
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
    throw NotImplementedException();
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
    throw NotImplementedException();
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

    if (isNull()) {
        return false;
    } else {
        return _sqlValue->equals(other);
    }
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

#else
static_assert(false);
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
