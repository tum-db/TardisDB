//
// Created by josef on 22.03.17.
//

#include "sql/SqlOperators.hpp"

#include <cmath>
#include <unordered_map>

#include "codegen/CodeGen.hpp"
#include "codegen/PhiNode.hpp"
#include "foundations/exceptions.hpp"
#include "foundations/LegacyTypes.hpp"
#include "sql/SqlUtils.hpp"
#include "sql/SqlCasts.hpp"
#include "sql/SqlOperators.hpp"
#include "sql/SqlType.hpp"
#include "sql/SqlValues.hpp"

using namespace Sql;

using TypeID = SqlType::TypeID;

namespace Sql {
namespace Operators {

/// The error of an overflow of a result of the type, as PostgreSQL names it
static std::string overflowMessage(SqlType type)
{
    switch (type.typeID) {
        case TypeID::IntegerID: return "integer out of range";
        case TypeID::LongIntegerID: return "bigint out of range";
        case TypeID::NumericID: return "numeric field overflow";
        default: return "arithmetic overflow";
    }
}

// generic binary operator wrapper function
template<llvm::Intrinsic::ID opIntrinsic>
llvm::Value * CreateOverflowBinOp(llvm::Value * lhs, llvm::Value * rhs, SqlType resultTy)
{
    auto & codeGen = getThreadLocalCodeGen();
    llvm::Module & module = codeGen.getCurrentModule();

    // intrinsic lookup
    llvm::Function * fun = llvm::Intrinsic::getDeclaration(&module,
        opIntrinsic, { lhs->getType() }); // overloaded on the type of the operands
    assert(fun);

    // performe the binary operation
    llvm::Value * result = codeGen->CreateCall(fun, {lhs, rhs});
    llvm::Value * opResult = codeGen->CreateExtractValue(result, 0, "opResult");
    llvm::Value * obit = codeGen->CreateExtractValue(result, 1, "obit");

    IfGen overflowCheck(obit);
    {
        genRuntimeError(overflowMessage(resultTy));
    }
    overflowCheck.EndIf();

    return opResult;
}

auto const CreateOverflowSAdd = &CreateOverflowBinOp<llvm::Intrinsic::sadd_with_overflow>;
auto const CreateOverflowSSub = &CreateOverflowBinOp<llvm::Intrinsic::ssub_with_overflow>;
auto const CreateOverflowSMul = &CreateOverflowBinOp<llvm::Intrinsic::smul_with_overflow>;

//-----------------------------------------------------------------------------
// add

static value_op_t sqlAddImpl(const Integer & lhs, const Integer & rhs, SqlType resultType)
{
    llvm::Value * raw = CreateOverflowSAdd(
        lhs.getLLVMValue(), rhs.getLLVMValue(), resultType);
    return value_op_t( Integer::fromRawValues({ raw }) );
}

static value_op_t sqlAddImpl(const LongInteger & lhs, const LongInteger & rhs, SqlType resultType)
{
    llvm::Value * raw = CreateOverflowSAdd(
        lhs.getLLVMValue(), rhs.getLLVMValue(), resultType);
    return value_op_t( LongInteger::fromRawValues({ raw }) );
}

static value_op_t sqlAddImpl(const Numeric & lhs, const Numeric & rhs, SqlType resultType)
{
    using cg_value_t = Numeric::cg_value_type;

    llvm::Value * lhsRaw = lhs.getLLVMValue();
    llvm::Value * rhsRaw = rhs.getLLVMValue();

    // adjust length and precision
    int diff = lhs.type.numericLayout.precision - rhs.type.numericLayout.precision;
    if (diff < 0) {
        cg_value_t shift( numericShifts[ std::abs(diff) ] );
        lhsRaw = CreateOverflowSMul(lhsRaw, shift.llvmValue, resultType);
    } else if (diff >  0) {
        cg_value_t shift( numericShifts[ diff ] );
        rhsRaw = CreateOverflowSMul(rhsRaw, shift.llvmValue, resultType);
    }

    // performe the arithmetic operation
    llvm::Value * raw = CreateOverflowSAdd(lhsRaw, rhsRaw, resultType);
    return value_op_t( Numeric::fromRawValues({ raw }, resultType) );
}

/// the arithmetic of doubles (no overflow checks: IEEE infinities, as PostgreSQL's float8 except for its error)
static value_op_t doubleArithmetic(const Value & lhs, const Value & rhs, llvm::Instruction::BinaryOps op)
{
    auto & codeGen = getThreadLocalCodeGen();
    return Double::fromRawValues({ codeGen->CreateBinOp(op, lhs.getLLVMValue(), rhs.getLLVMValue()) });
}

value_op_t sqlAdd(const Value & outerLhs, const Value & outerRhs)
{
    SqlType resultType;
    bool valid = inferAddTy(outerLhs.type, outerRhs.type, resultType);
    assert(valid);

    // the action that is performed if both input values are not null
    auto action = [&](const Value & lhs, const Value & rhs) {
        SqlType notNullableType = toNotNullableTy(resultType);

        // only values with the same type can be added
        if (lhs.type.typeID == TypeID::IntegerID && rhs.type.typeID == TypeID::IntegerID) {
            return sqlAddImpl(static_cast<const Integer &>(lhs), static_cast<const Integer &>(rhs), notNullableType);
        } else if (lhs.type.typeID == TypeID::LongIntegerID && rhs.type.typeID == TypeID::LongIntegerID) {
            return sqlAddImpl(static_cast<const LongInteger &>(lhs), static_cast<const LongInteger &>(rhs), notNullableType);
        } else if (lhs.type.typeID == TypeID::NumericID && rhs.type.typeID == TypeID::NumericID) {
            return sqlAddImpl(static_cast<const Numeric &>(lhs), static_cast<const Numeric &>(rhs), notNullableType);
        } else if (lhs.type.typeID == TypeID::DoubleID && rhs.type.typeID == TypeID::DoubleID) {
            return doubleArithmetic(lhs, rhs, llvm::Instruction::FAdd);
        } else {
            throw InvalidOperationException("attempt to add incompatible types");
        }
    };
    return Utils::nullHandler(action, outerLhs, outerRhs, resultType);
}

static bool isNumberType(SqlType type)
{
    return type.typeID == TypeID::IntegerID || type.typeID == TypeID::LongIntegerID ||
           type.typeID == TypeID::NumericID || type.typeID == TypeID::DoubleID;
}

/// integer or bigint
static bool isIntegerType(SqlType type)
{
    return type.typeID == TypeID::IntegerID || type.typeID == TypeID::LongIntegerID;
}

/// a double with a number (or two doubles) is a double
static bool inferDoubleTy(SqlType lhs, SqlType rhs, SqlType & result)
{
    if ((lhs.typeID == TypeID::DoubleID || rhs.typeID == TypeID::DoubleID) && isNumberType(lhs) && isNumberType(rhs)) {
        result = getDoubleTy(lhs.nullable || rhs.nullable);
        return true;
    }
    return false;
}

bool inferAddTy(SqlType lhs, SqlType rhs, SqlType & result)
{
    bool valid = true;

    if (lhs.typeID == TypeID::UnknownID && rhs.typeID == TypeID::UnknownID) {
        result = lhs;
        return true;
    } else if (lhs.typeID == TypeID::UnknownID) {
        lhs = rhs;
        lhs.nullable = true;
    } else if (rhs.typeID == TypeID::UnknownID) {
        rhs = lhs;
        rhs.nullable = true;
    }

    if (inferDoubleTy(lhs, rhs, result)) {
        return true;
    } else if (isIntegerType(lhs) && isIntegerType(rhs)) {
        result = (lhs.typeID == TypeID::LongIntegerID || rhs.typeID == TypeID::LongIntegerID) ? getLongIntegerTy() : getIntegerTy();
    } else if (isIntegerType(lhs) && rhs.typeID == TypeID::NumericID) {
        result = rhs;
    } else if (lhs.typeID == TypeID::NumericID && isIntegerType(rhs)) {
        result = lhs;
    } else if (lhs.typeID == TypeID::NumericID && rhs.typeID == TypeID::NumericID) {
        if (lhs.numericLayout.precision > rhs.numericLayout.precision) {
            result = lhs;
        } else {
            result = rhs;
        }
    } else {
        valid = false;
    }

    result.nullable = lhs.nullable || rhs.nullable;

    return valid;
}

//-----------------------------------------------------------------------------
// sub

static value_op_t sqlSubImpl(const Integer & lhs, const Integer & rhs, SqlType resultType)
{
    llvm::Value * raw = CreateOverflowSSub(
        lhs.getLLVMValue(), rhs.getLLVMValue(), resultType);
    return value_op_t( Integer::fromRawValues({ raw }) );
}

static value_op_t sqlSubImpl(const LongInteger & lhs, const LongInteger & rhs, SqlType resultType)
{
    llvm::Value * raw = CreateOverflowSSub(
        lhs.getLLVMValue(), rhs.getLLVMValue(), resultType);
    return value_op_t( LongInteger::fromRawValues({ raw }) );
}

static value_op_t sqlSubImpl(const Numeric & lhs, const Numeric & rhs, SqlType resultType)
{
    using cg_value_t = Numeric::cg_value_type;

    llvm::Value * lhsRaw = lhs.getLLVMValue();
    llvm::Value * rhsRaw = rhs.getLLVMValue();

    // adjust length and precision
    int diff = lhs.type.numericLayout.precision - rhs.type.numericLayout.precision;
    if (diff < 0) {
        cg_value_t shift( numericShifts[ std::abs(diff) ] );
        lhsRaw = CreateOverflowSMul(lhsRaw, shift.llvmValue, resultType);
    } else if (diff >  0) {
        cg_value_t shift( numericShifts[ diff ] );
        rhsRaw = CreateOverflowSMul(rhsRaw, shift.llvmValue, resultType);
    }

    // performe the arithmetic operation
    llvm::Value * raw = CreateOverflowSSub(lhsRaw, rhsRaw, resultType);
    return value_op_t( Numeric::fromRawValues({ raw }, resultType) );
}

value_op_t sqlSub(const Value & outerLhs, const Value & outerRhs)
{
    SqlType resultType;
    bool valid = inferSubTy(outerLhs.type, outerRhs.type, resultType);
    assert(valid);

    auto action = [&](const Value & lhs, const Value & rhs) {
        SqlType notNullableType = toNotNullableTy(resultType);

        // only values with the same type can be subtracted
        if (lhs.type.typeID == TypeID::IntegerID && rhs.type.typeID == TypeID::IntegerID) {
            return sqlSubImpl(static_cast<const Integer &>(lhs), static_cast<const Integer &>(rhs), notNullableType);
        } else if (lhs.type.typeID == TypeID::LongIntegerID && rhs.type.typeID == TypeID::LongIntegerID) {
            return sqlSubImpl(static_cast<const LongInteger &>(lhs), static_cast<const LongInteger &>(rhs), notNullableType);
        } else if (lhs.type.typeID == TypeID::NumericID && rhs.type.typeID == TypeID::NumericID) {
            return sqlSubImpl(static_cast<const Numeric &>(lhs), static_cast<const Numeric &>(rhs), notNullableType);
        } else if (lhs.type.typeID == TypeID::DoubleID && rhs.type.typeID == TypeID::DoubleID) {
            return doubleArithmetic(lhs, rhs, llvm::Instruction::FSub);
        } else {
            throw InvalidOperationException("attempt to subtract incompatible types");
        }
    };
    return Utils::nullHandler(action, outerLhs, outerRhs, resultType);
}

bool inferSubTy(SqlType lhs, SqlType rhs, SqlType & result)
{
    return inferAddTy(lhs, rhs, result);
}

//-----------------------------------------------------------------------------
// mul

static value_op_t sqlMulImpl(const Integer & lhs, const Integer & rhs, SqlType resultType)
{
    llvm::Value * raw = CreateOverflowSMul(
        lhs.getLLVMValue(), rhs.getLLVMValue(), resultType);
    return value_op_t( Integer::fromRawValues({ raw }) );
}

static value_op_t sqlMulImpl(const LongInteger & lhs, const LongInteger & rhs, SqlType resultType)
{
    llvm::Value * raw = CreateOverflowSMul(
        lhs.getLLVMValue(), rhs.getLLVMValue(), resultType);
    return value_op_t( LongInteger::fromRawValues({ raw }) );
}

static value_op_t sqlMulImpl(const Numeric & lhs, const Numeric & rhs, SqlType resultType)
{
    llvm::Value * lhsRaw = lhs.getLLVMValue();
    llvm::Value * rhsRaw = rhs.getLLVMValue();

    // performe the arithmetic operation
    llvm::Value * raw = CreateOverflowSMul(lhsRaw, rhsRaw, resultType);
    return value_op_t( Numeric::fromRawValues({ raw }, resultType) );
}

value_op_t sqlMul(const Value & outerLhs, const Value & outerRhs)
{
    SqlType resultType;
    bool valid = inferMulTy(outerLhs.type, outerRhs.type, resultType);
    assert(valid);

    auto action = [&](const Value & lhs, const Value & rhs) {
        SqlType notNullableType = toNotNullableTy(resultType);

        // only values with the same type can be multiplied
        if (lhs.type.typeID == TypeID::IntegerID && rhs.type.typeID == TypeID::IntegerID) {
            return sqlMulImpl(static_cast<const Integer &>(lhs), static_cast<const Integer &>(rhs), notNullableType);
        } else if (lhs.type.typeID == TypeID::LongIntegerID && rhs.type.typeID == TypeID::LongIntegerID) {
            return sqlMulImpl(static_cast<const LongInteger &>(lhs), static_cast<const LongInteger &>(rhs), notNullableType);
        } else if (lhs.type.typeID == TypeID::NumericID && rhs.type.typeID == TypeID::NumericID) {
            return sqlMulImpl(static_cast<const Numeric &>(lhs), static_cast<const Numeric &>(rhs), notNullableType);
        } else if (lhs.type.typeID == TypeID::DoubleID && rhs.type.typeID == TypeID::DoubleID) {
            return doubleArithmetic(lhs, rhs, llvm::Instruction::FMul);
        } else {
            throw InvalidOperationException("attempt to multiply incompatible types");
        }
    };
    return Utils::nullHandler(action, outerLhs, outerRhs, resultType);
}

bool inferMulTy(SqlType lhs, SqlType rhs, SqlType & result)
{
    bool valid = true;

    if (inferDoubleTy(lhs, rhs, result)) {
        return true;
    } else if (isIntegerType(lhs) && isIntegerType(rhs)) {
        result = (lhs.typeID == TypeID::LongIntegerID || rhs.typeID == TypeID::LongIntegerID) ? getLongIntegerTy() : getIntegerTy();
    } else if (isIntegerType(lhs) && rhs.typeID == TypeID::NumericID) {
        result = rhs;
    } else if (lhs.typeID == TypeID::NumericID && isIntegerType(rhs)) {
        result = lhs;
    } else if (lhs.typeID == TypeID::NumericID && rhs.typeID == TypeID::NumericID) {
        uint8_t length;
        uint8_t precision = lhs.numericLayout.precision + rhs.numericLayout.precision;

        if (lhs.numericLayout.length > rhs.numericLayout.length) {
            length = lhs.numericLayout.length + (precision - lhs.numericLayout.precision);
        } else if (lhs.numericLayout.length < rhs.numericLayout.length) {
            length = rhs.numericLayout.length + (precision - rhs.numericLayout.precision);
        } else {
            length = lhs.numericLayout.length + rhs.numericLayout.length;
        }

        result = Sql::getNumericTy(length, precision);
    } else {
        valid = false;
    }

    result.nullable = lhs.nullable || rhs.nullable;

    return valid;
}

//-----------------------------------------------------------------------------
// div

/// lhs / rhs of two integers (rhs not 0): the smallest one divided by -1 overflows (and would trap)
static llvm::Value * genCheckedSDiv(llvm::Value * lhs, llvm::Value * rhs)
{
    auto & codeGen = getThreadLocalCodeGen();
    unsigned bits = lhs->getType()->getIntegerBitWidth();
    llvm::Value * smallest = codeGen->CreateICmpEQ(lhs, codeGen->getInt(llvm::APInt::getSignedMinValue(bits)));
    llvm::Value * minusOne = codeGen->CreateICmpEQ(rhs, llvm::ConstantInt::getSigned(lhs->getType(), -1));
    llvm::Value * overflow = codeGen->CreateAnd(smallest, minusOne);
    IfGen check((cg_bool_t(overflow)));
    {
        genRuntimeError(bits == 32 ? "integer out of range" : "bigint out of range");
    }
    check.EndIf();
    // the quotient of -1 instead (the error ends the statement before)
    return codeGen->CreateSDiv(lhs, codeGen->CreateSelect(overflow, llvm::ConstantInt::get(lhs->getType(), 1), rhs));
}

static value_op_t sqlDivImpl(const Integer & lhs, const Integer & rhs, SqlType resultType)
{
    return value_op_t( Integer::fromRawValues({ genCheckedSDiv(lhs.getLLVMValue(), rhs.getLLVMValue()) }) );
}

static value_op_t sqlDivImpl(const LongInteger & lhs, const LongInteger & rhs, SqlType resultType)
{
    return value_op_t( LongInteger::fromRawValues({ genCheckedSDiv(lhs.getLLVMValue(), rhs.getLLVMValue()) }) );
}

static value_op_t sqlDivImpl(const Numeric & lhs, const Numeric & rhs, SqlType resultType)
{
    using cg_value_t = Numeric::cg_value_type;

    auto & codeGen = getThreadLocalCodeGen();
    llvm::Value * lhsRaw = lhs.getLLVMValue();
    llvm::Value * rhsRaw = rhs.getLLVMValue();

    // adjust length and precision
    int diff = lhs.type.numericLayout.precision - rhs.type.numericLayout.precision;
    if (diff <= 0) {
        cg_value_t shift( numericShifts[ std::abs(diff) + rhs.type.numericLayout.precision ] );
        lhsRaw = CreateOverflowSMul(lhsRaw, shift.llvmValue, resultType);
    } else if (diff >  0) {
        if (diff > rhs.type.numericLayout.precision) {
            cg_value_t shift( numericShifts[ diff - rhs.type.numericLayout.precision ] );
            rhsRaw = CreateOverflowSMul(rhsRaw, shift.llvmValue, resultType);
        } else if (diff < rhs.type.numericLayout.precision) {
            cg_value_t shift( numericShifts[ rhs.type.numericLayout.precision - diff] );
            lhsRaw = CreateOverflowSMul(lhsRaw, shift.llvmValue, resultType);
        }
    }

    // performe the arithmetic operation
    llvm::Value * raw = codeGen->CreateSDiv(lhsRaw, rhsRaw);
    return value_op_t( Numeric::fromRawValues({ raw }, resultType) );
}

value_op_t sqlDiv(const Value & outerLhs, const Value & outerRhs)
{
    SqlType resultType;
    bool valid = inferDivTy(outerLhs.type, outerRhs.type, resultType);
    assert(valid);

    auto action = [&](const Value & lhs, const Value & rhs) {
        auto & codeGen = getThreadLocalCodeGen();

        SqlType notNullableType = toNotNullableTy(resultType);

        llvm::Value * raw = rhs.getLLVMValue();
        cg_bool_t isZero(false);
        if (raw->getType()->isDoubleTy()) {
            isZero = cg_bool_t( codeGen->CreateFCmpOEQ(raw, llvm::ConstantFP::get(raw->getType(), 0.0)) );
        } else {
            llvm::Value * zero = codeGen->getIntN( raw->getType()->getPrimitiveSizeInBits(), 0 );
            isZero = cg_bool_t( codeGen->CreateICmpEQ(raw, zero) );
        }

        // a division by zero ends the statement: the error does not return
        llvm::Function * function = codeGen->GetInsertBlock()->getParent();
        llvm::BasicBlock * zeroBlock = llvm::BasicBlock::Create(codeGen.getLLVMContext(), "division_by_zero", function);
        llvm::BasicBlock * nonZeroBlock = llvm::BasicBlock::Create(codeGen.getLLVMContext(), "divisor_not_zero", function);
        codeGen->CreateCondBr(isZero.llvmValue, zeroBlock, nonZeroBlock);
        codeGen->SetInsertPoint(zeroBlock);
        genRuntimeError("division by zero");
        codeGen->CreateUnreachable();
        codeGen->SetInsertPoint(nonZeroBlock);

        // only values with the same type can be added
        if (lhs.type.typeID == TypeID::IntegerID && rhs.type.typeID == TypeID::IntegerID) {
            return sqlDivImpl(static_cast<const Integer &>(lhs), static_cast<const Integer &>(rhs), notNullableType);
        } else if (lhs.type.typeID == TypeID::LongIntegerID && rhs.type.typeID == TypeID::LongIntegerID) {
            return sqlDivImpl(static_cast<const LongInteger &>(lhs), static_cast<const LongInteger &>(rhs), notNullableType);
        } else if (lhs.type.typeID == TypeID::NumericID && rhs.type.typeID == TypeID::NumericID) {
            return sqlDivImpl(static_cast<const Numeric &>(lhs), static_cast<const Numeric &>(rhs), notNullableType);
        } else if (lhs.type.typeID == TypeID::DoubleID && rhs.type.typeID == TypeID::DoubleID) {
            return doubleArithmetic(lhs, rhs, llvm::Instruction::FDiv);
        } else {
            throw InvalidOperationException("attempt to divide incompatible types");
        }
    };
    return Utils::nullHandler(action, outerLhs, outerRhs, resultType);
}

bool inferDivTy(SqlType lhs, SqlType rhs, SqlType & result)
{
    bool valid = true;

    if (inferDoubleTy(lhs, rhs, result)) {
        return true;
    } else if (isIntegerType(lhs) && isIntegerType(rhs)) {
        result = (lhs.typeID == TypeID::LongIntegerID || rhs.typeID == TypeID::LongIntegerID) ? getLongIntegerTy() : getIntegerTy();
    } else if (isIntegerType(lhs) && rhs.typeID == TypeID::NumericID) {
        result = rhs;
    } else if (lhs.typeID == TypeID::NumericID && isIntegerType(rhs)) {
        result = lhs;
    } else if (lhs.typeID == TypeID::NumericID && rhs.typeID == TypeID::NumericID) {
        // div seems to take the max precision
        if (lhs.numericLayout.precision > rhs.numericLayout.precision) {
            result = lhs;
        } else {
            result = rhs;
        }
    } else {
        valid = false;
    }

    result.nullable = lhs.nullable || rhs.nullable;

    return valid;
}

//-----------------------------------------------------------------------------
// compare

value_op_t sqlCompare(const Value & outerLhs, const Value & outerRhs, ComparisonMode mode)
{
    assert(Sql::equals(outerLhs.type, outerRhs.type, SqlTypeEqualsMode::WithoutNullable));

    SqlType resultType;
    bool valid = inferCompareTy(outerLhs.type, outerRhs.type, resultType);
    assert(valid);

    auto action = [&](const Sql::Value & lhs, const Sql::Value & rhs) {
        cg_bool_t result = lhs.compare(rhs, mode);

        // convert to a SQL Bool
        return Sql::Bool::fromRawValues({ result });
    };
    return Utils::nullHandler(action, outerLhs, outerRhs, resultType);
}

bool inferCompareTy(SqlType lhs, SqlType rhs, SqlType & result)
{
    if (!Sql::equals(lhs, rhs, SqlTypeEqualsMode::WithoutNullable)) {
        return false;
    }

    result = Sql::getBoolTy(lhs.nullable || rhs.nullable);
    return true;
}

//-----------------------------------------------------------------------------
// casts

value_op_t sqlCast(const Value & value, SqlType newType)
{
    return Casts::cast(value, newType, Casts::Context::Explicit);
}

llvm::Value * genOverflowMul(llvm::Value * lhs, llvm::Value * rhs)
{
    return CreateOverflowSMul(lhs, rhs, getNumericTy(18, 0)); // the shifts of numerics
}

//-----------------------------------------------------------------------------
// logical and

/// The truth value and the null indicator of a boolean, which may be NULL
static std::pair<cg_bool_t, cg_bool_t> truthAndNull(const Value & value)
{
    if (isUnknown(value)) return {cg_bool_t(false), cg_bool_t(true)}; // the constant NULL
    if (isNullable(value)) {
        auto & nullable = static_cast<const NullableValue &>(value);
        return {cg_bool_t(nullable.getValue().getLLVMValue()), nullable.isNull()};
    }
    return {cg_bool_t(value.getLLVMValue()), cg_bool_t(false)};
}

value_op_t sqlAnd(const Value & lhs, const Value & rhs)
{
    assert(lhs.type.typeID == SqlType::TypeID::BoolID || isUnknown(lhs));
    assert(rhs.type.typeID == SqlType::TypeID::BoolID || isUnknown(rhs));

    if (!mayBeNull(lhs) && !mayBeNull(rhs)) {
        cg_bool_t lhsRaw(lhs.getLLVMValue());
        cg_bool_t rhsRaw(rhs.getLLVMValue());
        cg_bool_t result = lhsRaw && rhsRaw;
        return Sql::Bool::fromRawValues({result.llvmValue});
    }
    // three-valued, without branches: any false -> false, otherwise any null -> null, otherwise true
    auto [l, lNull] = truthAndNull(lhs);
    auto [r, rNull] = truthAndNull(rhs);
    cg_bool_t isFalse = (!l && !lNull) || (!r && !rNull);
    cg_bool_t isNull = !isFalse && (lNull || rNull);
    return Sql::NullableValue::create(Sql::Bool::fromRawValues({(!isFalse && !isNull).llvmValue}), isNull);
}

value_op_t sqlOr(const Value & lhs, const Value & rhs)
{
    assert(lhs.type.typeID == SqlType::TypeID::BoolID || isUnknown(lhs));
    assert(rhs.type.typeID == SqlType::TypeID::BoolID || isUnknown(rhs));

    if (!mayBeNull(lhs) && !mayBeNull(rhs)) {
        cg_bool_t lhsRaw(lhs.getLLVMValue());
        cg_bool_t rhsRaw(rhs.getLLVMValue());
        cg_bool_t result = lhsRaw || rhsRaw;
        return Sql::Bool::fromRawValues({result.llvmValue});
    }
    // three-valued, without branches: any true -> true, otherwise any null -> null, otherwise false
    auto [l, lNull] = truthAndNull(lhs);
    auto [r, rNull] = truthAndNull(rhs);
    cg_bool_t isTrue = (l && !lNull) || (r && !rNull);
    cg_bool_t isNull = !isTrue && (lNull || rNull);
    return Sql::NullableValue::create(Sql::Bool::fromRawValues({isTrue.llvmValue}), isNull);
}

value_op_t sqlNot(const Value & value)
{
    assert(value.type.typeID == SqlType::TypeID::BoolID);

    auto action = [](const Value & innerValue) {
        cg_bool_t booleanValue( innerValue.getLLVMValue() );
        return value_op_t( Bool::fromRawValues({ !booleanValue }) );
    };
    return Utils::nullHandler(action, value, value.type);
}

        /// Throws an error of the generated code (see genRuntimeError)
        static void throwRuntimeError(const char * message)
        {
            throw ArithmeticException(message);
        }

        void genRuntimeError(const std::string & message)
        {
            auto & codeGen = getThreadLocalCodeGen();
            llvm::Value * text = codeGen->CreateGlobalString(message);
            llvm::FunctionType * type = llvm::FunctionType::get(codeGen->getVoidTy(), {LLVMCompat::getInt8PtrTy(codeGen.getLLVMContext())}, false);
            codeGen.CreateCall(&throwRuntimeError, type, std::vector<llvm::Value *>{text});
        }

} // end namespace Operators
} // end namespace Sql
