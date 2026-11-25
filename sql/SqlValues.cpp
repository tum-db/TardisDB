#include "sql/SqlValues.hpp"

#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/DerivedTypes.h>
#include "codegen/LLVMCompat.hpp"

#include <functional>
#include <limits>
#include <map>
#include <mutex>

#include "codegen/CodeGen.hpp"
#include "codegen/PhiNode.hpp"
#include "foundations/exceptions.hpp"
#include "foundations/LegacyTypes.hpp"
#include "foundations/StringPool.hpp"
#include "native/sql/Array.hpp"
#include "native/sql/SqlValues.hpp"
#include "sql/SqlType.hpp"
#include "utils/general.hpp"
#include "utils/llvm.hpp"

namespace Sql {

// the pointer of a store, typed for LLVM versions without opaque pointers (a no-op otherwise)
static llvm::Value * typedPointer(llvm::Value * ptr, llvm::Type * elementTy)
{
    return getThreadLocalCodeGen()->CreatePointerCast(ptr, llvm::PointerType::getUnqual(elementTy));
}


//-----------------------------------------------------------------------------
// common functions

// the same as shown in Types.hpp
static cg_hash_t genIntHash(llvm::Value * value)
{
    cg_hash_t x(value); // zero extend
    cg_hash_t r(88172645463325252ul);
    r = r ^ x;
    r = r ^ (r << 13ul);
    r = r ^ (r >> 7ul);
    r = r ^ (r << 17ul);
    return r;
}

static cg_hash_t genStringHash(CodeGen & codeGen, cg_ptr8_t & str, cg_size_t len)
{
    auto & funcGen = codeGen.getCurrentFunctionGen();

#ifdef __APPLE__
    cg_hash_t result(0ull);
#else
    cg_hash_t result(0ul);
#endif

    // iterate over all characters
#ifdef __APPLE__
    LoopGen loopGen(funcGen, {{"index", cg_size_t(0ull)}, {"result", result}});
#else
    LoopGen loopGen(funcGen, {{"index", cg_size_t(0ul)}, {"result", result}});
#endif
    cg_size_t index(loopGen.getLoopVar(0));
    {
        LoopBodyGen bodyGen(loopGen);

        cg_ptr8_t ptr = str + index;
        cg_char_t c( codeGen->CreateLoad(codeGen->getInt8Ty(), ptr) );

//        Functions::genPrintfCall("hash iteration: %lu - char: %c\n", index, c);

        // apply the same hash algorithm as in Types.hpp
        cg_hash_t x( codeGen->CreateZExt( c, result.llvmValue->getType() ) );
        result = cg_hash_t(loopGen.getLoopVar(1));
        result = ((result << 5ul) | (result >> 27ul)) ^ x;
    }
    cg_size_t nextIndex(index + 1ul);
    loopGen.loopDone(nextIndex < len, {nextIndex, result});

    return result;
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
        case SqlType::TypeID::LongIntegerID:
            return LongInteger::castString(str);
        case SqlType::TypeID::VarcharID:
            return Varchar::castString(str, type);
        case SqlType::TypeID::CharID:
            return Char::castString(str, type);
        case SqlType::TypeID::NumericID:
            return Numeric::castString(str, type);
        case SqlType::TypeID::DateID:
            return Date::castString(str);
        case SqlType::TypeID::TimestampID:
            return Timestamp::castString(str);
        case SqlType::TypeID::TextID:
            return Text::castString(str, type);
        case SqlType::TypeID::DoubleID:
            return Double::castString(str);
        case SqlType::TypeID::ArrayID:
            return Array::castString(str, type);
        default:
            unreachable();
    }
}

value_op_t Value::castString(cg_ptr8_t str, cg_size_t length, SqlType type)
{
    assert(!type.nullable);

    switch (type.typeID) {
        case SqlType::TypeID::UnknownID:
            return UnknownValue::create();
        case SqlType::TypeID::BoolID:
            return Bool::castString(str, length);
        case SqlType::TypeID::IntegerID:
            return Integer::castString(str, length);
        case SqlType::TypeID::VarcharID:
            return Varchar::castString(str, length, type);
        case SqlType::TypeID::CharID:
            return Char::castString(str, length, type);
        case SqlType::TypeID::NumericID:
            return Numeric::castString(str, length, type);
        case SqlType::TypeID::DateID:
            return Date::castString(str, length);
        case SqlType::TypeID::TimestampID:
            return Timestamp::castString(str, length);
        case SqlType::TypeID::TextID:
            return Text::castString(str, length, type);
        case SqlType::TypeID::DoubleID:
            return Double::castString(str, length);
        default:
            unreachable();
    }
}

value_op_t Value::fromRawValues(const std::vector<llvm::Value *> & values, SqlType type)
{
    value_op_t sqlValue;

    SqlType notNullType = type;
    notNullType.nullable = false;

    switch (type.typeID) {
        case SqlType::TypeID::UnknownID:
            return UnknownValue::create();
        case SqlType::TypeID::BoolID:
            sqlValue = Bool::fromRawValues(values);
            break;
        case SqlType::TypeID::IntegerID:
            sqlValue = Integer::fromRawValues(values);
            break;
        case SqlType::TypeID::VarcharID:
            sqlValue = Varchar::fromRawValues(values, notNullType);
            break;
        case SqlType::TypeID::CharID:
            sqlValue = Char::fromRawValues(values, notNullType);
            break;
        case SqlType::TypeID::NumericID:
            sqlValue = Numeric::fromRawValues(values, notNullType);
            break;
        case SqlType::TypeID::DateID:
            sqlValue = Date::fromRawValues(values);
            break;
        case SqlType::TypeID::TimestampID:
            sqlValue = Timestamp::fromRawValues(values);
            break;
        case SqlType::TypeID::TextID:
            sqlValue = Text::fromRawValues(values, notNullType);
            break;
        case SqlType::TypeID::DoubleID:
            sqlValue = Double::fromRawValues(values);
            break;
        case SqlType::TypeID::ArrayID:
            sqlValue = Array::fromRawValues(values, notNullType);
            break;
        default:
            unreachable();
    }

    if (type.nullable) {
        return NullableValue::create(std::move(sqlValue), cg_bool_t(values.back()));
    } else {
        return sqlValue;
    }
}

value_op_t Value::load(llvm::Value * ptr, SqlType type)
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
        case SqlType::TypeID::LongIntegerID:
            return LongInteger::load(ptr);
        case SqlType::TypeID::VarcharID:
            return Varchar::load(ptr, type);
        case SqlType::TypeID::CharID:
            return Char::load(ptr, type);
        case SqlType::TypeID::NumericID:
            return Numeric::load(ptr, type);
        case SqlType::TypeID::DateID:
            return Date::load(ptr);
        case SqlType::TypeID::TimestampID:
            return Timestamp::load(ptr);
        case SqlType::TypeID::TextID:
            return Text::load(ptr, type);
        case SqlType::TypeID::DoubleID:
            return Double::load(ptr);
        case SqlType::TypeID::ArrayID:
            return Array::load(ptr, type);
        default:
            unreachable();
    }
}

llvm::Value * Value::getLLVMValue() const
{
    return _llvmValue;
}

std::vector<llvm::Value *> Value::getRawValues() const
{
    return { _llvmValue };
}

size_t Value::getSize() const
{
    return getValueSize(type);
}

llvm::Type * Value::getLLVMType() const
{
    return toLLVMTy(type);
}

cg_bool_t Value::compare(const Value & other, ComparisonMode mode) const
{
    auto & codeGen = getThreadLocalCodeGen();
    cg_bool_t cmp(false);

    // map to the corrosponding LLVM instructions
    switch (mode) {
        case ComparisonMode::less:
            cmp = cg_bool_t( codeGen->CreateICmpSLT( _llvmValue, other.getLLVMValue() ) );
            break;
        case ComparisonMode::leq:
            cmp = cg_bool_t( codeGen->CreateICmpSLE( _llvmValue, other.getLLVMValue() ) );
            break;
        case ComparisonMode::eq:
            cmp = cg_bool_t( codeGen->CreateICmpEQ( _llvmValue, other.getLLVMValue() ) );
            break;
        case ComparisonMode::geq:
            cmp = cg_bool_t( codeGen->CreateICmpSGE( _llvmValue, other.getLLVMValue() ) );
            break;
        case ComparisonMode::gtr:
            cmp = cg_bool_t( codeGen->CreateICmpSGT( _llvmValue, other.getLLVMValue() ) );
            break;
        default:
            unreachable();
    }

    return cmp;
}

//-----------------------------------------------------------------------------
// Integer

Integer::Integer(llvm::Value * value) :
    Value(getIntegerTy())
{
    this->_llvmValue = value;
}

Integer::Integer(value_type constantValue) :
        Value(getIntegerTy())
{
    _llvmValue = cg_value_type(constantValue);
}

value_op_t Integer::clone() const
{
    Value * cloned = new Integer(_llvmValue);
    return value_op_t(cloned);
}

value_op_t Integer::castString(const std::string & str)
{
    Integer::value_type raw = castStringToIntegerValue(str.c_str(), static_cast<uint32_t>(str.length()));
    llvm::Value * value = cg_value_type(raw);
    value_op_t sqlValue( new Integer(value) );
    return sqlValue;
}

value_op_t Integer::castString(cg_ptr8_t str, cg_size_t length)
{
    cg_value_type raw = genCastStringToIntegerValueCall(str, length);
    value_op_t sqlValue( new Integer(raw) );
    return sqlValue;
}

value_op_t Integer::fromRawValues(const std::vector<llvm::Value *> & values)
{
    assert(!values.empty());
    return value_op_t(new Integer(values.front()));
}

value_op_t Integer::load(llvm::Value * ptr)
{
    auto & codeGen = getThreadLocalCodeGen();
    llvm::Type * integerTy = cg_value_type::getType();

    llvm::Value * value = codeGen->CreateLoad(integerTy, ptr);
//    Functions::genPrintfCall("Integer::load ptr: %p value: %d\n", ptr, value);
    value_op_t sqlValue( new Integer(value) );
    return sqlValue;
}

void Integer::store(llvm::Value * ptr) const
{
//    Functions::genPrintfCall("Integer::store ptr: %p value %d\n", ptr, _llvmValue);
    auto & codeGen = getThreadLocalCodeGen();
    codeGen->CreateStore(_llvmValue, typedPointer(ptr, _llvmValue->getType()));
}

cg_hash_t Integer::hash() const
{
    return genIntHash(_llvmValue);
}

void Integer::accept(ValueVisitor & visitor)
{
    visitor.visit(*this);
}

cg_bool_t Integer::equals(const Value & other) const
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
// Long Integer

LongInteger::LongInteger(llvm::Value * value) : Value(getLongIntegerTy()) {
    this->_llvmValue = value;
}

LongInteger::LongInteger(value_type constantValue) : Value(getLongIntegerTy()) {
    _llvmValue = cg_value_type(constantValue);
}

value_op_t LongInteger::clone() const {
    Value *cloned = new LongInteger(_llvmValue);
    return value_op_t(cloned);
}

value_op_t LongInteger::castString(const std::string & str) {
    // signed, unlike the cast of strings at run time
    size_t end = 0;
    LongInteger::value_type raw = std::stoll(str, &end);
    if (end != str.size()) throw InvalidOperationException("invalid longinteger '" + str + "'");
    llvm::Value * value = cg_value_type(raw);
    value_op_t sqlValue( new LongInteger(value) );
    return sqlValue;
}

value_op_t LongInteger::fromRawValues(const std::vector<llvm::Value *> & values) {
    assert(!values.empty());
    return value_op_t(new LongInteger(values.front()));
}

value_op_t LongInteger::load(llvm::Value * ptr) {
    auto & codeGen = getThreadLocalCodeGen();
    llvm::Type * integerTy = cg_value_type::getType();

    llvm::Value * value = codeGen->CreateLoad(integerTy, ptr);
    //Functions::genPrintfCall("Integer::load ptr: %p\n", ptr, value);
    value_op_t sqlValue( new LongInteger(value) );
    return sqlValue;
}

void LongInteger::store(llvm::Value * ptr) const {
//    Functions::genPrintfCall("Integer::store ptr: %p value %d\n", ptr, _llvmValue);
    auto & codeGen = getThreadLocalCodeGen();
    codeGen->CreateStore(_llvmValue, typedPointer(ptr, _llvmValue->getType()));
}

cg_hash_t LongInteger::hash() const {
    return genIntHash(_llvmValue);
}

void LongInteger::accept(ValueVisitor & visitor) {
    visitor.visit(*this);
}

cg_bool_t LongInteger::equals(const Value & other) const {
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
    llvm::Type * numericTy = cg_value_type::getType();

    llvm::Value * value = codeGen->CreateLoad(numericTy, ptr);
    value_op_t sqlValue( new Numeric(type, value) );
    return sqlValue;
}

void Numeric::store(llvm::Value * ptr) const
{
    auto & codeGen = getThreadLocalCodeGen();
    codeGen->CreateStore(_llvmValue, typedPointer(ptr, _llvmValue->getType()));
}

cg_hash_t Numeric::hash() const
{
    // can be used for all integral types
    return genIntHash(_llvmValue);
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
    llvm::Type * boolTy = codeGen->getInt1Ty();

    llvm::Value * value = codeGen->CreateLoad(boolTy, ptr);
    value_op_t sqlValue( new Bool(value) );
    return sqlValue;
}

void Bool::store(llvm::Value * ptr) const
{
    auto & codeGen = getThreadLocalCodeGen();
    codeGen->CreateStore(_llvmValue, typedPointer(ptr, _llvmValue->getType()));
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

/// memcmp over the common length, then the lengths
static int32_t compareStringBytes(const char * s1, size_t len1, const char * s2, size_t len2)
{
    int diff = std::memcmp(s1, s2, std::min(len1, len2));
    if (diff != 0) return diff < 0 ? -1 : 1;
    return (len1 < len2) ? -1 : (len1 > len2 ? 1 : 0);
}

static cg_bool_t compareResult(cg_int_t diff, ComparisonMode mode)
{
    switch (mode) {
        case ComparisonMode::less: return diff < cg_int_t(0);
        case ComparisonMode::leq: return diff <= cg_int_t(0);
        case ComparisonMode::eq: return diff == cg_int_t(0);
        case ComparisonMode::geq: return diff >= cg_int_t(0);
        case ComparisonMode::gtr: return diff > cg_int_t(0);
    }
    unreachable();
}

cg_bool_t BasicString::compareBuffers(const BasicString & str1, const BasicString & str2, ComparisonMode mode)
{
    auto & codeGen = getThreadLocalCodeGen();
    llvm::FunctionType * funcTy = llvm::TypeBuilder<int32_t (const char *, size_t, const char *, size_t), false>::get(codeGen.getLLVMContext());
    llvm::CallInst * result = codeGen.CreateCall(&compareStringBytes, funcTy, {
            cg_voidptr_t(str1.getLLVMValue()), cg_size_t(str1.getLength()),
            cg_voidptr_t(str2.getLLVMValue()), cg_size_t(str2.getLength())});
    return compareResult(cg_int_t(llvm::cast<llvm::Value>(result)), mode);
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
    llvm::Type * charTy = toLLVMTy(toNotNullableTy(type));

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
        codeGen->CreateStore(_llvmValue, typedPointer(ptr, _llvmValue->getType()));
    } else {
        llvm::Type * valueTy = getLLVMType();
        ptr = typedPointer(ptr, valueTy);

        // store the Char's length
        codeGen->CreateStore(_length, codeGen->CreateStructGEP(valueTy, ptr, 0));

        // store the Char's data
        llvm::Value * strPtr = codeGen->CreateStructGEP(valueTy, ptr, 1);
#if LLVM_VERSION_MAJOR < 7
        codeGen->CreateMemCpy(strPtr, _llvmValue, _length, 0);
#else
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
        LLVMCompat::createMemCpy(codeGen.getIRBuilder(), strPtr, _llvmValue, _length);
#endif
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
    if (isNullable(other)) {
        throw NotImplementedException("Char::compare with a nullable value");
    }
    if (mode == ComparisonMode::eq) {
        return equals(other);
    }
    auto & codeGen = getThreadLocalCodeGen();
    if (type.length == 1) {
        // the character itself, as an unsigned byte
        llvm::Value * l = _llvmValue, * r = other.getLLVMValue();
        switch (mode) {
            case ComparisonMode::less: return cg_bool_t(codeGen->CreateICmpULT(l, r));
            case ComparisonMode::leq: return cg_bool_t(codeGen->CreateICmpULE(l, r));
            case ComparisonMode::geq: return cg_bool_t(codeGen->CreateICmpUGE(l, r));
            case ComparisonMode::gtr: return cg_bool_t(codeGen->CreateICmpUGT(l, r));
            default: unreachable();
        }
    }
    return compareBuffers(*this, static_cast<const BasicString &>(other), mode);
}

//-----------------------------------------------------------------------------
// Text

    /*Text::Text(SqlType type, llvm::Value * value, llvm::Value * length) :
            BasicString(type, value, length)
    {

    }*/

    Text::Text(llvm::Value * value) :
            BasicString(getTextTy(), value, getThreadLocalCodeGen()->getInt8(16))
    {

    }

    Text::Text(SqlType type, llvm::Value * value) :
            BasicString(type, value, getThreadLocalCodeGen()->getInt8(16))
    {
        assert(type.typeID == SqlType::TypeID::TextID && !type.nullable);
    }

    value_op_t Text::clone() const
    {
        return value_op_t( new Text(type, _llvmValue) );
    }

    void storeTextGen(char *dest, const uint8_t * bytes, size_t len) {
        if (len > 15) {
            std::unique_ptr<uint8_t[]> data(new uint8_t[len]);
            std::memcpy(data.get(), bytes, len);
            auto & storedStr = StringPool::instance().put(sql_string_t(len, std::move(data)));
            uint8_t * beginPtr = storedStr.second.get();
            uint8_t * endPtr = beginPtr+len;

            uintptr_t first_value = reinterpret_cast<uintptr_t>(beginPtr);
            first_value ^= static_cast<uintptr_t>(1) << (8*sizeof(uintptr_t)-1);

            memcpy(dest,(char*)&first_value,8);
            memcpy(dest + 8, &endPtr, 8);
        } else {
            dest[0] = (char) len;
            memcpy(&dest[1],bytes,len);
        }
    }

    // the 16 bytes of a text: inline, the length and up to 15 bytes; else (the leftmost bit of the first
    // word set) the address of the first byte, tagged, and of the end (as Native::Sql::Text)
    static const uintptr_t textTag = static_cast<uintptr_t>(1) << (8 * sizeof(uintptr_t) - 1);

    void* textBegin(char *stringPtr) {
        uintptr_t words[2];
        std::memcpy(words, stringPtr, sizeof(words));
        if (words[0] & textTag) return reinterpret_cast<void *>(words[0] ^ textTag);
        return &stringPtr[1];
    }

    uint64_t getLengthText(char *stringPtr) {
        uintptr_t words[2];
        std::memcpy(words, stringPtr, sizeof(words));
        if (words[0] & textTag) return words[1] - (words[0] ^ textTag);
        return static_cast<uint8_t>(stringPtr[0]);
    }

    value_op_t Text::castString(const std::string & str, SqlType type)
    {
        // a constant global of the 16 bytes (inline or of the string pool), which every function of the
        // module reads; instructions here could land in another function than the one that uses it
        auto & codeGen = getThreadLocalCodeGen();
        uint64_t words[2];
        Native::Sql::Text::castString(str)->store(words);
        auto * int64Ty = llvm::Type::getInt64Ty(codeGen.getLLVMContext());
        auto * arrayTy = llvm::ArrayType::get(int64Ty, 2);
        llvm::Constant * init = llvm::ConstantArray::get(arrayTy, {llvm::ConstantInt::get(int64Ty, words[0]), llvm::ConstantInt::get(int64Ty, words[1])});
        auto * global = new llvm::GlobalVariable(codeGen.getCurrentModuleGen().getModule(), arrayTy, true,
                                                 llvm::GlobalValue::PrivateLinkage, init, "text");
        return value_op_t( new Text(type, global) );
    }

    value_op_t Text::castString(cg_ptr8_t str, cg_size_t length, SqlType type)
    {
        auto & codeGen = getThreadLocalCodeGen();
        cg_voidptr_t arrayPtr = cg_voidptr_t( llvm::cast<llvm::Value>(codeGen->CreateAlloca(llvm::Type::getInt64Ty(codeGen.getLLVMContext()),2)) );
        auto & context = codeGen.getLLVMContext();
        llvm::FunctionType * funcTy = llvm::TypeBuilder<void (const char *, const char*, size_t), false>::get(context);
        llvm::CallInst * result = codeGen.CreateCall(&storeTextGen, funcTy, {arrayPtr, str, length});
        return value_op_t( new Text(type, arrayPtr) );
    }

    value_op_t Text::fromRawValues(const std::vector<llvm::Value *> & values, SqlType type)
    {
        return value_op_t(new Text(values.front()));
    }

    value_op_t Text::load(llvm::Value * ptr, SqlType type)
    {
        auto & codeGen = getThreadLocalCodeGen();

        //llvm::Type * llvmType = toLLVMTy(type);
        cg_voidptr_t arrayPtr = cg_voidptr_t( llvm::cast<llvm::Value>(codeGen->CreateAlloca(llvm::Type::getInt64Ty(codeGen.getLLVMContext()),2)) );

        //llvm::Value * begin = codeGen->CreateStructGEP(llvmType, ptr, 0);
        //llvm::Value * strPtr = codeGen->CreateGlobalString(std::string("AAAAAAAAAAAAAAAA"));
        LLVMCompat::createMemCpy(codeGen.getIRBuilder(), arrayPtr, ptr, 16);

        /*llvm::FunctionType * funcTy = llvm::TypeBuilder<int (void *), false>::get(codeGen.getLLVMContext());
        llvm::Function * func = LLVMCompat::getOrInsertFunction(getThreadLocalCodeGen().getCurrentModuleGen().getModule(), "getLengthText", funcTy);
        getThreadLocalCodeGen().getCurrentModuleGen().addFunctionMapping(func,(void *)&getLengthText);
        llvm::CallInst * result = codeGen->CreateCall(func, { strPtr });
        cg_u64_t length = cg_u64_t( llvm::cast<llvm::Value>(result) );*/

        return value_op_t( new Text(type, arrayPtr) );
    }

    void Text::store(llvm::Value * ptr) const
    {
        auto & codeGen = getThreadLocalCodeGen();

        llvm::Type * valueTy = getLLVMType();

        // store the Char's data
        //llvm::Value * strPtr = codeGen->CreateStructGEP(valueTy, ptr, 0);
#if LLVM_VERSION_MAJOR < 7
        codeGen->CreateMemCpy(strPtr, _llvmValue, _length, 0);
#else
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

        //strValue = codeGen->CreateLoad(charTy, ptr);

        LLVMCompat::createMemCpy(codeGen.getIRBuilder(), ptr, _llvmValue, 16);
#endif
    }

    /// The length and the first byte of a text value (a pointer to its 16 bytes), as generated calls
    static cg_size_t genTextLength(llvm::Value * text)
    {
        auto & codeGen = getThreadLocalCodeGen();
        llvm::FunctionType * funcTy = llvm::TypeBuilder<size_t (void *), false>::get(codeGen.getLLVMContext());
        llvm::Function * func = LLVMCompat::getOrInsertFunction(codeGen.getCurrentModuleGen().getModule(), "getLengthText", funcTy);
        codeGen.getCurrentModuleGen().addFunctionMapping(func, (void *) &getLengthText);
        return cg_size_t(llvm::cast<llvm::Value>(codeGen->CreateCall(func, {text})));
    }

    static cg_voidptr_t genTextBegin(llvm::Value * text)
    {
        auto & codeGen = getThreadLocalCodeGen();
        llvm::FunctionType * funcTy = llvm::TypeBuilder<void * (void *), false>::get(codeGen.getLLVMContext());
        llvm::Function * func = LLVMCompat::getOrInsertFunction(codeGen.getCurrentModuleGen().getModule(), "textBegin", funcTy);
        codeGen.getCurrentModuleGen().addFunctionMapping(func, (void *) &textBegin);
        return cg_voidptr_t(llvm::cast<llvm::Value>(codeGen->CreateCall(func, {text})));
    }

    cg_hash_t Text::hash() const
    {
        auto & codeGen = getThreadLocalCodeGen();
        cg_size_t length = genTextLength(_llvmValue);
        cg_voidptr_t begin = genTextBegin(_llvmValue);
        return genStringHash(codeGen, begin, length);
    }

    void Text::accept(ValueVisitor & visitor)
    {
        visitor.visit(*this);
    }

    cg_bool_t Text::equals(const Value & other) const
    {
        auto & codeGen = getThreadLocalCodeGen();

        if (!Sql::equals(type, other.type, SqlTypeEqualsMode::WithoutNullable)) {
            return cg_bool_t(false);
        }

        if (isNullable(other)) {
            return other.equals(*this);
        }

        llvm::Value * otherText = static_cast<const Text &>(other).getLLVMValue();
        cg_size_t thisLength = genTextLength(_llvmValue);
        cg_bool_t isLengthEqual = genTextLength(otherText) == thisLength;

        IfGen check( codeGen.getCurrentFunctionGen(), isLengthEqual, {{"isEqual", cg_bool_t(false)}} );
        {
            cg_int_t compareResult = Functions::genMemcmpCall(genTextBegin(otherText), genTextBegin(_llvmValue), thisLength);
            check.setVar(0, cg_bool_t(compareResult == cg_int_t(0)));
        }
        check.EndIf();
        return cg_bool_t(check.getResult(0));
    }

    /// as ORDER BY: by the bytes (collation C); with a NULL, false
    cg_bool_t Text::compare(const Value & other, ComparisonMode mode) const
    {
        if (isNullable(other)) {
            // the nullable side compares, the other way round
            ComparisonMode turned = mode == ComparisonMode::less ? ComparisonMode::gtr : mode == ComparisonMode::gtr ? ComparisonMode::less :
                                    mode == ComparisonMode::leq ? ComparisonMode::geq : mode == ComparisonMode::geq ? ComparisonMode::leq : mode;
            return other.compare(*this, turned);
        }
        if (other.type.typeID != SqlType::TypeID::TextID) throw NotImplementedException("Text::compare with " + getName(other.type));
        if (mode == ComparisonMode::eq) return equals(other);
        auto & codeGen = getThreadLocalCodeGen();
        llvm::Value * otherText = static_cast<const Text &>(other).getLLVMValue();
        llvm::FunctionType * funcTy = llvm::TypeBuilder<int32_t (const char *, size_t, const char *, size_t), false>::get(codeGen.getLLVMContext());
        llvm::CallInst * result = codeGen.CreateCall(&compareStringBytes, funcTy, {
                genTextBegin(_llvmValue), genTextLength(_llvmValue), genTextBegin(otherText), genTextLength(otherText)});
        return compareResult(cg_int_t(llvm::cast<llvm::Value>(result)), mode);
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

    llvm::Type * varcharTy = toLLVMTy(toNotNullableTy(type));
    llvm::Value * strPtr = codeGen->CreateStructGEP(varcharTy, ptr, 1);
    llvm::Value * length = loadStringLength(ptr, type);
    return value_op_t( new Varchar(type, strPtr, length) );
}

void Varchar::store(llvm::Value * ptr) const
{
    auto & codeGen = getThreadLocalCodeGen();
    llvm::Type * valueTy = getLLVMType();
    ptr = typedPointer(ptr, valueTy);

    // store the Varchar's length
    codeGen->CreateStore(_length, codeGen->CreateStructGEP(valueTy, ptr, 0));

    // store the Varchar's data
    llvm::Value * strPtr = codeGen->CreateStructGEP(valueTy, ptr, 1);

//    Functions::genPrintfCall("store Varchar at: %p\n", strPtr);
#if LLVM_VERSION_MAJOR < 7
    codeGen->CreateMemCpy(strPtr, _llvmValue, _length, 0);
#else
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
    LLVMCompat::createMemCpy(codeGen.getIRBuilder(), strPtr, _llvmValue, _length);
#endif
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
    if (isNullable(other)) {
        throw NotImplementedException("Varchar::compare with a nullable value");
    }
    if (mode == ComparisonMode::eq) {
        return equals(other);
    }
    return compareBuffers(*this, static_cast<const BasicString &>(other), mode);
}

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
    llvm::Type * dateTy = cg_value_type::getType();

    llvm::Value * value = codeGen->CreateLoad(dateTy, ptr);
    value_op_t sqlValue( new Date(value) );
    return sqlValue;
}

void Date::store(llvm::Value * ptr) const
{
    auto & codeGen = getThreadLocalCodeGen();
    codeGen->CreateStore(_llvmValue, typedPointer(ptr, _llvmValue->getType()));
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
    llvm::Type * timestampTy = cg_value_type::getType();

    llvm::Value * value = codeGen->CreateLoad(timestampTy, ptr);
    value_op_t sqlValue( new Timestamp(value) );
    return sqlValue;
}

void Timestamp::store(llvm::Value * ptr) const
{
    auto & codeGen = getThreadLocalCodeGen();
    codeGen->CreateStore(_llvmValue, typedPointer(ptr, _llvmValue->getType()));
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
// Double

llvm::Value * genEntryAlloca(llvm::Type * type)
{
    auto & codeGen = getThreadLocalCodeGen();
    llvm::Function * function = codeGen->GetInsertBlock()->getParent();
    llvm::BasicBlock & entry = function->getEntryBlock();
    llvm::IRBuilder<> builder(&entry, entry.begin());
    return builder.CreateAlloca(type);
}

static double parseDoubleBytes(const char * str, size_t length)
{
    return Native::Sql::parseDouble(std::string(str, length));
}

Double::Double(llvm::Value * value) :
        Value(getDoubleTy())
{
    _llvmValue = value;
}

value_op_t Double::castString(const std::string & str)
{
    auto & codeGen = getThreadLocalCodeGen();
    return value_op_t(new Double(llvm::ConstantFP::get(llvm::Type::getDoubleTy(codeGen.getLLVMContext()), Native::Sql::parseDouble(str))));
}

value_op_t Double::castString(cg_ptr8_t str, cg_size_t length)
{
    auto & codeGen = getThreadLocalCodeGen();
    llvm::FunctionType * funcTy = llvm::FunctionType::get(llvm::Type::getDoubleTy(codeGen.getLLVMContext()),
            {cg_voidptr_t::getType(), cg_size_t::getType()}, false);
    llvm::CallInst * result = codeGen.CreateCall(&parseDoubleBytes, funcTy, {cg_voidptr_t(str), length.getValue()});
    return value_op_t(new Double(result));
}

value_op_t Double::fromRawValues(const std::vector<llvm::Value *> & values)
{
    assert(!values.empty());
    return value_op_t(new Double(values.front()));
}

value_op_t Double::load(llvm::Value * ptr)
{
    auto & codeGen = getThreadLocalCodeGen();
    llvm::Type * doubleTy = llvm::Type::getDoubleTy(codeGen.getLLVMContext());
    return value_op_t(new Double(codeGen->CreateLoad(doubleTy, typedPointer(ptr, doubleTy))));
}

value_op_t Double::clone() const
{
    return value_op_t(new Double(_llvmValue));
}

void Double::store(llvm::Value * ptr) const
{
    auto & codeGen = getThreadLocalCodeGen();
    codeGen->CreateStore(_llvmValue, typedPointer(ptr, _llvmValue->getType()));
}

cg_hash_t Double::hash() const
{
    // -0 hashes as 0 (x + 0.0), all NaNs as one
    auto & codeGen = getThreadLocalCodeGen();
    llvm::Type * doubleTy = _llvmValue->getType();
    llvm::Value * canonical = codeGen->CreateFAdd(_llvmValue, llvm::ConstantFP::get(doubleTy, 0.0));
    llvm::Value * isNaN = codeGen->CreateFCmpUNO(_llvmValue, _llvmValue);
    canonical = codeGen->CreateSelect(isNaN, llvm::ConstantFP::getNaN(doubleTy), canonical);
    return genIntHash(codeGen->CreateBitCast(canonical, codeGen->getInt64Ty()));
}

void Double::accept(ValueVisitor & visitor)
{
    visitor.visit(*this);
}

cg_bool_t Double::equals(const Value & other) const
{
    if (!Sql::equals(type, other.type, SqlTypeEqualsMode::WithoutNullable)) {
        return cg_bool_t(false);
    }
    if (isNullable(other)) {
        return other.equals(*this);
    }
    return compare(other, ComparisonMode::eq);
}

cg_bool_t Double::compare(const Value & other, ComparisonMode mode) const
{
    // the order of PostgreSQL: NaN equals NaN and is greater than all other values
    auto & codeGen = getThreadLocalCodeGen();
    llvm::Value * a = _llvmValue, * b = other.getLLVMValue();
    llvm::Value * aNaN = codeGen->CreateFCmpUNO(a, a), * bNaN = codeGen->CreateFCmpUNO(b, b);
    llvm::Value * bothNaN = codeGen->CreateAnd(aNaN, bNaN);
    llvm::Value * eq = codeGen->CreateOr(codeGen->CreateFCmpOEQ(a, b), bothNaN);
    // a < b: b is NaN and a is not, or both are numbers and a < b
    llvm::Value * less = codeGen->CreateOr(codeGen->CreateAnd(bNaN, codeGen->CreateNot(aNaN)), codeGen->CreateFCmpOLT(a, b));
    llvm::Value * greater = codeGen->CreateOr(codeGen->CreateAnd(aNaN, codeGen->CreateNot(bNaN)), codeGen->CreateFCmpOGT(a, b));
    switch (mode) {
        case ComparisonMode::less: return cg_bool_t(less);
        case ComparisonMode::leq: return cg_bool_t(codeGen->CreateOr(less, eq));
        case ComparisonMode::eq: return cg_bool_t(eq);
        case ComparisonMode::geq: return cg_bool_t(codeGen->CreateOr(greater, eq));
        case ComparisonMode::gtr: return cg_bool_t(greater);
    }
    unreachable();
}

//-----------------------------------------------------------------------------
// Array

static llvm::Type * handleType()
{
    return llvm::ArrayType::get(llvm::Type::getInt64Ty(getThreadLocalCodeGen().getLLVMContext()), 2);
}

Array::Array(SqlType type, llvm::Value * value) :
        Value(type)
{
    assert(type.typeID == SqlType::TypeID::ArrayID && !type.nullable);
    _llvmValue = value;
}

value_op_t Array::castString(const std::string & str, SqlType type)
{
    // a constant: the representation is in the string pool
    Native::Sql::Arrays::Handle handle = Native::Sql::Arrays::parse(str, type, nullptr);
    auto * int64Ty = llvm::Type::getInt64Ty(getThreadLocalCodeGen().getLLVMContext());
    llvm::Constant * words[2] = {
            llvm::ConstantInt::get(int64Ty, reinterpret_cast<uint64_t>(handle.data)),
            llvm::ConstantInt::get(int64Ty, handle.length)};
    return value_op_t(new Array(type, llvm::ConstantArray::get(llvm::cast<llvm::ArrayType>(handleType()), words)));
}

value_op_t Array::fromRawValues(const std::vector<llvm::Value *> & values, SqlType type)
{
    assert(!values.empty());
    return value_op_t(new Array(type, values.front()));
}

value_op_t Array::load(llvm::Value * ptr, SqlType type)
{
    auto & codeGen = getThreadLocalCodeGen();
    return value_op_t(new Array(type, codeGen->CreateLoad(handleType(), typedPointer(ptr, handleType()))));
}

value_op_t Array::clone() const
{
    return value_op_t(new Array(type, _llvmValue));
}

void Array::store(llvm::Value * ptr) const
{
    auto & codeGen = getThreadLocalCodeGen();
    codeGen->CreateStore(_llvmValue, typedPointer(ptr, handleType()));
}

llvm::Value * Array::spill() const
{
    auto & codeGen = getThreadLocalCodeGen();
    llvm::Value * slot = genEntryAlloca(handleType());
    codeGen->CreateStore(_llvmValue, slot);
    return slot;
}

cg_hash_t Array::hash() const
{
    auto & codeGen = getThreadLocalCodeGen();
    llvm::FunctionType * funcTy = llvm::FunctionType::get(codeGen->getInt64Ty(), {cg_voidptr_t::getType(), codeGen->getInt64Ty()}, false);
    llvm::CallInst * result = codeGen.CreateCall(&Native::Sql::Arrays::tardisArrayHash, funcTy,
            {spill(), codeGen->getInt64(Native::Sql::Arrays::typeBits(type))});
    return cg_hash_t(llvm::cast<llvm::Value>(result));
}

void Array::accept(ValueVisitor & visitor)
{
    visitor.visit(*this);
}

cg_bool_t Array::equals(const Value & other) const
{
    if (!Sql::equals(type, other.type, SqlTypeEqualsMode::WithoutNullable)) {
        return cg_bool_t(false);
    }
    if (isNullable(other)) {
        return other.equals(*this);
    }
    auto & codeGen = getThreadLocalCodeGen();
    llvm::FunctionType * funcTy = llvm::FunctionType::get(codeGen->getInt1Ty(), {cg_voidptr_t::getType(), cg_voidptr_t::getType(), codeGen->getInt64Ty()}, false);
    llvm::CallInst * result = codeGen.CreateCall(&Native::Sql::Arrays::tardisArrayEquals, funcTy,
            {spill(), static_cast<const Array &>(other).spill(), codeGen->getInt64(Native::Sql::Arrays::typeBits(type))});
    return cg_bool_t(llvm::cast<llvm::Value>(result));
}

value_op_t genArrayCall(Native::Sql::Arrays::Function function, Native::Sql::Arrays::Arena * arena, SqlType resultType,
                        const std::vector<const Value *> & arguments)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();
    const uint32_t n = static_cast<uint32_t>(arguments.size());
    llvm::Type * ptrTy = cg_voidptr_t::getType();
    llvm::Type * i8Ty = codeGen->getInt8Ty();
    llvm::Type * i64Ty = codeGen->getInt64Ty();
    const uint32_t slots = std::max<uint32_t>(n, 1);
    llvm::Type * pointersTy = llvm::ArrayType::get(ptrTy, slots);
    llvm::Type * nullsTy = llvm::ArrayType::get(i8Ty, slots);
    llvm::Type * typesTy = llvm::ArrayType::get(i64Ty, slots);
    llvm::Value * pointers = genEntryAlloca(pointersTy);
    llvm::Value * nulls = genEntryAlloca(nullsTy);
    llvm::Value * types = genEntryAlloca(typesTy);

    for (uint32_t i = 0; i < n; ++i) {
        const Value & argument = *arguments[i];
        SqlType type = toNotNullableTy(argument.type);
        llvm::Value * isNull = codeGen->getInt1(false);
        const Value * inner = &argument;
        if (isUnknown(argument)) {
            isNull = codeGen->getInt1(true);
            inner = nullptr;
        } else if (isNullable(argument)) {
            auto & nullable = static_cast<const NullableValue &>(argument);
            isNull = nullable.isNull().llvmValue;
            inner = &nullable.getValue();
        }
        llvm::Value * slot = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy));
        if (inner != nullptr) {
            slot = genEntryAlloca(toLLVMTy(type));
            inner->store(slot);
            slot = codeGen->CreatePointerCast(slot, ptrTy);
        }
        codeGen->CreateStore(slot, codeGen->CreateConstInBoundsGEP2_32(pointersTy, pointers, 0, i));
        codeGen->CreateStore(codeGen->CreateZExt(isNull, i8Ty), codeGen->CreateConstInBoundsGEP2_32(nullsTy, nulls, 0, i));
        codeGen->CreateStore(llvm::ConstantInt::get(i64Ty, Native::Sql::Arrays::typeBits(inner ? type : getUnknownTy())),
                             codeGen->CreateConstInBoundsGEP2_32(typesTy, types, 0, i));
    }

    SqlType notNullResult = toNotNullableTy(resultType);
    llvm::Value * result = genEntryAlloca(toLLVMTy(notNullResult));
    llvm::FunctionType * funcTy = llvm::FunctionType::get(codeGen->getInt1Ty(),
            {codeGen->getInt32Ty(), ptrTy, i64Ty, ptrTy, codeGen->getInt32Ty(), ptrTy, ptrTy, ptrTy}, false);
    llvm::CallInst * isNull = codeGen.CreateCall(&Native::Sql::Arrays::tardisArrayCall, funcTy, {
            codeGen->getInt32(static_cast<uint32_t>(function)),
            cg_voidptr_t::fromRawPointer(arena),
            llvm::ConstantInt::get(i64Ty, Native::Sql::Arrays::typeBits(notNullResult)),
            codeGen->CreatePointerCast(result, ptrTy),
            codeGen->getInt32(n),
            codeGen->CreatePointerCast(pointers, ptrTy),
            codeGen->CreatePointerCast(nulls, ptrTy),
            codeGen->CreatePointerCast(types, ptrTy)});
    (void) context;

    value_op_t value = Value::load(result, notNullResult);
    if (!resultType.nullable) return value;
    return NullableValue::create(std::move(value), cg_bool_t(llvm::cast<llvm::Value>(isNull)));
}

cg_bool_t Array::compare(const Value & other, ComparisonMode mode) const
{
    auto & codeGen = getThreadLocalCodeGen();
    llvm::FunctionType * funcTy = llvm::FunctionType::get(codeGen->getInt32Ty(), {cg_voidptr_t::getType(), cg_voidptr_t::getType(), codeGen->getInt64Ty()}, false);
    llvm::CallInst * result = codeGen.CreateCall(&Native::Sql::Arrays::tardisArrayCompare, funcTy,
            {spill(), static_cast<const Array &>(getAssociatedValue(other)).spill(), codeGen->getInt64(Native::Sql::Arrays::typeBits(type))});
    return compareResult(cg_int_t(llvm::cast<llvm::Value>(result)), mode);
}

//-----------------------------------------------------------------------------
// UnknownValue

UnknownValue::UnknownValue() :
        Value(getUnknownTy())
{ }

value_op_t UnknownValue::create()
{
    return value_op_t(new UnknownValue);
}

std::vector<llvm::Value *> UnknownValue::getRawValues() const
{
    return { };
}

void UnknownValue::store(llvm::Value * ptr) const
{
    // nop
}

cg_hash_t UnknownValue::hash() const
{
    // arbitrary constant
    return cg_hash_t(0x2c8e4eecfc696c50ul);
}

value_op_t UnknownValue::clone() const
{
    return create();
}

void UnknownValue::accept(ValueVisitor & visitor)
{
    visitor.visit(*this);
}

cg_bool_t UnknownValue::equals(const Value & other) const
{
    return cg_bool_t(false);
}

cg_bool_t UnknownValue::compare(const Value & other, ComparisonMode mode) const
{
    return cg_bool_t(false);
}

//-----------------------------------------------------------------------------
// NullableValue (with external null indicator)


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

/// \returns The bytes of a NULL of the nullable type, which live as long as the process: the zeroed
/// value and the null indicator set
static const void * nullBytes(SqlType type)
{
    static std::mutex latch;
    static std::map<std::string, std::unique_ptr<uint8_t[]>> buffers;
    auto & dataLayout = getThreadLocalCodeGen().getDefaultDataLayout();
    auto * structTy = llvm::cast<llvm::StructType>(toLLVMTy(type));
    std::lock_guard<std::mutex> lock(latch);
    auto & buffer = buffers[getName(type)];
    if (!buffer) {
        buffer = std::make_unique<uint8_t[]>(dataLayout.getTypeAllocSize(structTy)); // zeroed
        buffer[dataLayout.getStructLayout(structTy)->getElementOffset(1)] = 1;
    }
    return buffer.get();
}

value_op_t NullableValue::getNull(SqlType type)
{
    type = toNullableTy(type);
    return load(createPointerValue(nullBytes(type), toLLVMTy(type)), type);
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


} // end namespace Sql
