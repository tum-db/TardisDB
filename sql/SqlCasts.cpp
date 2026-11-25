// The casts between the SQL types (see SqlCasts.hpp)

#include "sql/SqlCasts.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "codegen/CodeGen.hpp"
#include "foundations/exceptions.hpp"
#include "foundations/LegacyTypes.hpp"
#include "sql/SqlOperators.hpp"
#include "sql/SqlUtils.hpp"

namespace Sql {
namespace Casts {

using TypeID = SqlType::TypeID;

namespace {

bool isNumber(SqlType t)
{
    return t.typeID == TypeID::IntegerID || t.typeID == TypeID::LongIntegerID || t.typeID == TypeID::NumericID || t.typeID == TypeID::DoubleID;
}

bool isString(SqlType t)
{
    return t.typeID == TypeID::CharID || t.typeID == TypeID::VarcharID || t.typeID == TypeID::TextID;
}

//-----------------------------------------------------------------------------
// numbers

/// a double rounded to the nearest integer, which must fit into the bits: half to even (as PostgreSQL
/// casts to integers) or half away from zero (as it casts to numerics)
llvm::Value * roundDoubleToInt(llvm::Value * value, unsigned bits, bool halfToEven = true)
{
    auto & codeGen = getThreadLocalCodeGen();
    llvm::Function * rint = llvm::Intrinsic::getDeclaration(&codeGen.getCurrentModuleGen().getModule(),
                                                            halfToEven ? llvm::Intrinsic::rint : llvm::Intrinsic::round, {value->getType()});
    llvm::Value * rounded = codeGen->CreateCall(rint, {value});
    double limit = std::ldexp(1.0, static_cast<int>(bits) - 1);
    llvm::Value * inRange = codeGen->CreateAnd(
            codeGen->CreateFCmpOGE(rounded, llvm::ConstantFP::get(value->getType(), -limit)),
            codeGen->CreateFCmpOLT(rounded, llvm::ConstantFP::get(value->getType(), limit)));
    IfGen check(cg_bool_t(codeGen->CreateNot(inRange)));
    {
        Operators::genOverflowException();
    }
    check.EndIf();
    return codeGen->CreateFPToSI(rounded, codeGen->getIntNTy(bits));
}

llvm::Value * toDouble(llvm::Value * raw)
{
    auto & codeGen = getThreadLocalCodeGen();
    return codeGen->CreateSIToFP(raw, codeGen->getDoubleTy());
}

/// a bigint (i64) as an integer, which must fit
llvm::Value * narrowToInteger(llvm::Value * raw)
{
    auto & codeGen = getThreadLocalCodeGen();
    llvm::Value * truncated = codeGen->CreateTrunc(raw, codeGen->getInt32Ty());
    IfGen check(cg_bool_t(codeGen->CreateICmpNE(codeGen->CreateSExt(truncated, codeGen->getInt64Ty()), raw)));
    {
        Operators::genOverflowException();
    }
    check.EndIf();
    return truncated;
}

/// raw / divisor (a power of ten), rounded half away from zero (as PostgreSQL rounds a numeric)
llvm::Value * roundingDiv(llvm::Value * raw, uint64_t divisor)
{
    auto & codeGen = getThreadLocalCodeGen();
    if (divisor == 1) return raw;
    llvm::Type * type = raw->getType();
    llvm::Value * half = llvm::ConstantInt::get(type, divisor / 2);
    llvm::Value * negative = codeGen->CreateICmpSLT(raw, llvm::ConstantInt::get(type, 0));
    llvm::Value * adjusted = codeGen->CreateSelect(negative, codeGen->CreateSub(raw, half), codeGen->CreateAdd(raw, half));
    return codeGen->CreateSDiv(adjusted, llvm::ConstantInt::get(type, divisor));
}

value_op_t castInteger(const Value & value, SqlType target, Context)
{
    auto & codeGen = getThreadLocalCodeGen();
    llvm::Value * raw = value.getLLVMValue();
    switch (target.typeID) {
        case TypeID::LongIntegerID:
            return LongInteger::fromRawValues({ codeGen->CreateSExt(raw, codeGen->getInt64Ty()) });
        case TypeID::DoubleID:
            return Double::fromRawValues({ toDouble(raw) });
        case TypeID::NumericID: {
            raw = codeGen->CreateSExt(raw, Numeric::cg_value_type::getType());
            Numeric::cg_value_type shift( numericShifts[target.numericLayout.precision] );
            return Numeric::fromRawValues({ Operators::genOverflowMul(raw, shift) }, target);
        }
        default:
            unreachable();
    }
}

value_op_t castLongInteger(const Value & value, SqlType target, Context)
{
    llvm::Value * raw = value.getLLVMValue();
    switch (target.typeID) {
        case TypeID::IntegerID:
            return Integer::fromRawValues({ narrowToInteger(raw) });
        case TypeID::NumericID: {
            Numeric::cg_value_type shift( numericShifts[target.numericLayout.precision] );
            return Numeric::fromRawValues({ Operators::genOverflowMul(raw, shift) }, target);
        }
        case TypeID::DoubleID:
            return Double::fromRawValues({ toDouble(raw) });
        default:
            unreachable();
    }
}

value_op_t castNumeric(const Value & value, SqlType target, Context)
{
    auto & codeGen = getThreadLocalCodeGen();
    llvm::Value * raw = value.getLLVMValue();
    uint8_t scale = value.type.numericLayout.precision;
    switch (target.typeID) {
        case TypeID::DoubleID: {
            llvm::Value * d = toDouble(raw);
            return Double::fromRawValues({ codeGen->CreateFDiv(d, llvm::ConstantFP::get(d->getType(), static_cast<double>(numericShifts[scale]))) });
        }
        case TypeID::LongIntegerID:
            return LongInteger::fromRawValues({ roundingDiv(raw, numericShifts[scale]) });
        case TypeID::IntegerID:
            return Integer::fromRawValues({ narrowToInteger(roundingDiv(raw, numericShifts[scale])) });
        case TypeID::NumericID: {
            // another scale: digits behind the point are rounded away or appended
            uint8_t targetScale = target.numericLayout.precision;
            if (scale > targetScale) {
                raw = roundingDiv(raw, numericShifts[scale - targetScale]);
            } else if (scale < targetScale) {
                Numeric::cg_value_type factor( numericShifts[targetScale - scale] );
                raw = Operators::genOverflowMul(raw, factor);
            }
            return Numeric::fromRawValues({ raw }, target);
        }
        default:
            unreachable();
    }
}

value_op_t castDouble(const Value & value, SqlType target, Context)
{
    auto & codeGen = getThreadLocalCodeGen();
    llvm::Value * raw = value.getLLVMValue();
    switch (target.typeID) {
        case TypeID::IntegerID:
            return Integer::fromRawValues({ roundDoubleToInt(raw, 32) });
        case TypeID::LongIntegerID:
            return LongInteger::fromRawValues({ roundDoubleToInt(raw, 64) });
        case TypeID::NumericID: {
            llvm::Value * scaled = codeGen->CreateFMul(raw, llvm::ConstantFP::get(raw->getType(), static_cast<double>(numericShifts[target.numericLayout.precision])));
            return Numeric::fromRawValues({ roundDoubleToInt(scaled, 64, false) }, target);
        }
        default:
            unreachable();
    }
}

//-----------------------------------------------------------------------------
// strings

/// A slot in the entry block of the generated function: made once per call, not once per tuple (an
/// alloca in a loop would grow the stack with every iteration)
llvm::Value * entryAlloca(llvm::Type * type, unsigned count)
{
    auto & codeGen = getThreadLocalCodeGen();
    llvm::Function * function = codeGen->GetInsertBlock()->getParent();
    llvm::BasicBlock & entry = function->getEntryBlock();
    llvm::IRBuilder<> builder(&entry, entry.begin());
    return builder.CreateAlloca(type, builder.getInt32(count));
}

/// the length without the trailing spaces (of a character(n), which PostgreSQL drops in a cast)
size_t trimmedLength(const char * bytes, size_t length)
{
    while (length > 0 && bytes[length - 1] == ' ') --length;
    return length;
}

/// the 16 bytes of a text (see storeTextGen)
void storeText(char * dest, const char * bytes, size_t length)
{
    storeTextGen(dest, reinterpret_cast<const uint8_t *>(bytes), length);
}

struct Bytes {
    cg_ptr8_t begin;
    cg_size_t length;
};

/// The bytes of a string value; those of a character(n) without its trailing spaces
Bytes stringBytes(const Value & value)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();
    Bytes bytes;
    if (value.type.typeID == TypeID::TextID) {
        llvm::Value * text = value.getLLVMValue();
        bytes.begin = cg_ptr8_t(llvm::cast<llvm::Value>(codeGen.CreateCall(&textBegin, llvm::TypeBuilder<void * (void *), false>::get(context), {cg_ptr8_t(text)})));
        bytes.length = cg_size_t(llvm::cast<llvm::Value>(codeGen.CreateCall(&getLengthText, llvm::TypeBuilder<size_t (void *), false>::get(context), {cg_ptr8_t(text)})));
        return bytes;
    }
    const BasicString & string = static_cast<const BasicString &>(value);
    if (value.type.typeID == TypeID::CharID && value.type.length == 1) {
        // a character(1) is the byte itself
        llvm::Value * slot = entryAlloca(codeGen->getInt8Ty(), 1);
        codeGen->CreateStore(string.getLLVMValue(), slot);
        bytes.begin = cg_ptr8_t(slot);
        bytes.length = cg_size_t(1ul);
    } else {
        bytes.begin = cg_ptr8_t(string.getLLVMValue());
        bytes.length = cg_size_t(codeGen->CreateZExt(string.getLength(), cg_size_t::getType()));
    }
    if (value.type.typeID == TypeID::CharID) {
        bytes.length = cg_size_t(llvm::cast<llvm::Value>(codeGen.CreateCall(&trimmedLength, llvm::TypeBuilder<size_t (const char *, size_t), false>::get(context),
                                                                           {bytes.begin, bytes.length})));
    }
    return bytes;
}

/// At most the length of the target: an explicit cast truncates, any other sets the overflow flag
/// (PostgreSQL: "value too long for type character varying(n)")
cg_size_t fitLength(cg_size_t length, SqlType target, Context context)
{
    auto & codeGen = getThreadLocalCodeGen();
    llvm::Value * limit = cg_size_t(static_cast<size_t>(target.length));
    llvm::Value * tooLong = codeGen->CreateICmpUGT(length, limit);
    if (context != Context::Explicit) {
        IfGen check((cg_bool_t(tooLong)));
        {
            Operators::genOverflowException();
            Functions::genPrintfCall("value too long for type " + typeName(target) + "\n");
        }
        check.EndIf();
    }
    return cg_size_t(codeGen->CreateSelect(tooLong, limit, length));
}

value_op_t castString(const Value & value, SqlType target, Context context)
{
    auto & codeGen = getThreadLocalCodeGen();
    Bytes bytes = stringBytes(value);
    switch (target.typeID) {
        case TypeID::TextID: {
            llvm::Value * text = entryAlloca(codeGen->getInt64Ty(), 2);
            codeGen.CreateCall(&storeText, llvm::TypeBuilder<void (char *, const char *, size_t), false>::get(codeGen.getLLVMContext()),
                               {cg_ptr8_t(text), bytes.begin, bytes.length});
            return value_op_t( new Text(target, text) );
        }
        case TypeID::VarcharID:
            return Varchar::castString(bytes.begin, fitLength(bytes.length, target, context), target);
        case TypeID::CharID: {
            cg_size_t length = fitLength(bytes.length, target, context);
            if (target.length == 1) {
                // the byte itself, a space for the empty string (character(1) pads)
                llvm::Value * empty = codeGen->CreateICmpEQ(length, cg_size_t(0ul));
                llvm::Value * byte = codeGen->CreateLoad(codeGen->getInt8Ty(), bytes.begin);
                return Char::fromRawValues({ codeGen->CreateSelect(empty, codeGen->getInt8(' '), byte), codeGen->getInt8(1) }, target);
            }
            return Char::castString(bytes.begin, length, target);
        }
        default:
            unreachable();
    }
}

//-----------------------------------------------------------------------------
// the tables

const std::vector<Cast> integerCasts = {
        {TypeID::LongIntegerID, Context::Implicit, &castInteger},
        {TypeID::NumericID, Context::Implicit, &castInteger},
        {TypeID::DoubleID, Context::Implicit, &castInteger},
};
const std::vector<Cast> longIntegerCasts = {
        {TypeID::NumericID, Context::Implicit, &castLongInteger},
        {TypeID::DoubleID, Context::Implicit, &castLongInteger},
        {TypeID::IntegerID, Context::Assignment, &castLongInteger},
};
const std::vector<Cast> numericCasts = {
        {TypeID::NumericID, Context::Implicit, &castNumeric},
        {TypeID::DoubleID, Context::Implicit, &castNumeric},
        {TypeID::IntegerID, Context::Assignment, &castNumeric},
        {TypeID::LongIntegerID, Context::Assignment, &castNumeric},
};
const std::vector<Cast> doubleCasts = {
        {TypeID::IntegerID, Context::Assignment, &castDouble},
        {TypeID::LongIntegerID, Context::Assignment, &castDouble},
        {TypeID::NumericID, Context::Assignment, &castDouble},
};
const std::vector<Cast> charCasts = {
        {TypeID::CharID, Context::Implicit, &castString},
        {TypeID::VarcharID, Context::Implicit, &castString},
        {TypeID::TextID, Context::Implicit, &castString},
};
const std::vector<Cast> varcharCasts = {
        {TypeID::VarcharID, Context::Implicit, &castString},
        {TypeID::TextID, Context::Implicit, &castString},
        {TypeID::CharID, Context::Assignment, &castString},
};
const std::vector<Cast> textCasts = {
        {TypeID::VarcharID, Context::Assignment, &castString},
        {TypeID::CharID, Context::Assignment, &castString},
};
const std::vector<Cast> noCasts;

} // namespace

const std::vector<Cast> & castTable(TypeID source)
{
    switch (source) {
        case TypeID::IntegerID: return integerCasts;
        case TypeID::LongIntegerID: return longIntegerCasts;
        case TypeID::NumericID: return numericCasts;
        case TypeID::DoubleID: return doubleCasts;
        case TypeID::CharID: return charCasts;
        case TypeID::VarcharID: return varcharCasts;
        case TypeID::TextID: return textCasts;
        default: return noCasts;
    }
}

const Cast * findCast(SqlType source, SqlType target)
{
    for (const Cast & cast : castTable(source.typeID)) {
        if (cast.target == target.typeID) return &cast;
    }
    return nullptr;
}

bool castContext(SqlType source, SqlType target, Context & context)
{
    if (Sql::equals(source, target, SqlTypeEqualsMode::WithoutNullable)) {
        context = Context::Implicit;
        return true;
    }
    const Cast * cast = findCast(source, target);
    if (cast == nullptr) return false;
    context = cast->context;
    bool narrows = false;
    if (source.typeID == TypeID::NumericID && target.typeID == TypeID::NumericID) {
        narrows = target.numericLayout.precision < source.numericLayout.precision;
    } else if ((source.typeID == TypeID::CharID || source.typeID == TypeID::VarcharID) &&
               (target.typeID == TypeID::CharID || target.typeID == TypeID::VarcharID)) {
        narrows = target.length < source.length;
    }
    if (narrows && context == Context::Implicit) context = Context::Assignment;
    return true;
}

bool canCast(SqlType source, SqlType target, Context context)
{
    Context needed;
    return castContext(source, target, needed) && needed <= context;
}

bool commonType(SqlType a, SqlType b, SqlType & result)
{
    a = toNotNullableTy(a);
    b = toNotNullableTy(b);
    if (a.typeID == TypeID::UnknownID || b.typeID == TypeID::UnknownID) return false;
    if (Sql::equals(a, b, SqlTypeEqualsMode::WithoutNullable)) {
        result = a;
        return true;
    }
    if (isNumber(a) && isNumber(b)) {
        if (a.typeID == TypeID::NumericID && b.typeID == TypeID::NumericID) {
            uint8_t scale = std::max(a.numericLayout.precision, b.numericLayout.precision);
            int integers = std::max(a.numericLayout.length - a.numericLayout.precision, b.numericLayout.length - b.numericLayout.precision);
            result = getNumericTy(static_cast<uint8_t>(std::min(18, integers + scale)), scale);
            return true;
        }
        // integer < bigint < numeric < double precision
        auto rank = [](SqlType t) {
            switch (t.typeID) {
                case TypeID::IntegerID: return 1;
                case TypeID::LongIntegerID: return 2;
                case TypeID::NumericID: return 3;
                default: return 4;
            }
        };
        result = rank(a) >= rank(b) ? a : b;
        return true;
    }
    if (isString(a) && isString(b)) {
        uint32_t length = std::max(a.length, b.length);
        if (a.typeID == TypeID::TextID || b.typeID == TypeID::TextID) {
            result = getTextTy();
        } else {
            // also two characters of different lengths: without their trailing spaces, as PostgreSQL compares them
            result = getVarcharTy(length);
        }
        return true;
    }
    return false;
}

value_op_t cast(const Value & value, SqlType target, Context context)
{
    SqlType castType = toNotNullableTy(target);
    if (value.type.typeID == TypeID::UnknownID) {
        return NullableValue::getNull(castType);
    }
    if (Sql::equals(value.type, castType, SqlTypeEqualsMode::WithoutNullable)) {
        return value.clone();
    }
    const Cast * entry = findCast(value.type, castType);
    if (entry == nullptr) {
        throw ArithmeticException("cannot cast type " + typeName(value.type) + " to " + typeName(castType));
    }
    auto action = [&](const Value & v) {
        return entry->method(v, castType, context);
    };
    return Utils::nullHandler(action, value, castType);
}

std::string typeName(SqlType t)
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
        case TypeID::ArrayID: return typeName(getElementTy(t)) + "[]";
        case TypeID::UnknownID: return "unknown";
    }
    return "unknown";
}

} // end namespace Casts
} // end namespace Sql
