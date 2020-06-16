//
// Created by josef on 22.03.17.
//

#include "sql/SqlOperators.hpp"

#include <unordered_map>

#include "codegen/CodeGen.hpp"
#include "codegen/PhiNode.hpp"
#include "foundations/exceptions.hpp"
#include "foundations/LegacyTypes.hpp"
#include "foundations/QueryContext.hpp"
#include "sql/SqlUtils.hpp"
#include "sql/SqlType.hpp"
#include "sql/SqlValues.hpp"

using namespace Sql;

using TypeID = SqlType::TypeID;

namespace Sql {
namespace Operators {

using overflow_handler_t = std::function<void()>;

// generic binary operator wrapper function
template<llvm::Intrinsic::ID opIntrinsic>
llvm::Value * CreateOverflowBinOp(llvm::Value * lhs, llvm::Value * rhs, SqlType resultTy, overflow_handler_t handler)
{
    auto & codeGen = getThreadLocalCodeGen();
    llvm::Module & module = codeGen.getCurrentModule();

    // intrinsic lookup
    llvm::Function * fun = llvm::Intrinsic::getDeclaration(&module,
        opIntrinsic, { lhs->getType(), rhs->getType() });
    assert(fun);

    // performe the binary operation
    llvm::Value * result = codeGen->CreateCall(fun, {lhs, rhs});
    llvm::Value * opResult = codeGen->CreateExtractValue(result, 0, "opResult");
    llvm::Value * obit = codeGen->CreateExtractValue(result, 1, "obit");

    // the additional overflow check required for internal null indicators
#ifdef USE_INTERNAL_NULL_INDICATOR
    llvm::Value * nullIndicator = Sql::getNullIndicator(resultTy);
    if (nullIndicator == nullptr) {
        throw InvalidOperationException("type does not provide a null indicator");
    }

    llvm::Value * isResultNull;
    if (nullIndicator->getType()->isIntegerTy()) {
        // result == internal null indicator
        isResultNull = codeGen->CreateICmpEQ(opResult, nullIndicator);
    } else {
        throw NotImplementedException();
    }

    obit = codeGen->CreateOr(obit, isResultNull);
#endif // USE_INTERNAL_NULL_INDICATOR

    IfGen overflowCheck(obit);
    {
        handler();
        Functions::genPrintfCall("arithmetic overflow\n");
    }
    overflowCheck.EndIf();

    return opResult;
}

auto const CreateOverflowSAdd = &CreateOverflowBinOp<llvm::Intrinsic::sadd_with_overflow>;
//auto const CreateOverflowUAdd = &CreateOverflowBinOp<llvm::Intrinsic::uadd_with_overflow>;
auto const CreateOverflowSSub = &CreateOverflowBinOp<llvm::Intrinsic::ssub_with_overflow>;
//auto const CreateOverflowUSub = &CreateOverflowBinOp<llvm::Intrinsic::usub_with_overflow>;
auto const CreateOverflowSMul = &CreateOverflowBinOp<llvm::Intrinsic::smul_with_overflow>;
//auto const CreateOverflowUMul = &CreateOverflowBinOp<llvm::Intrinsic::umul_with_overflow>;

//-----------------------------------------------------------------------------
// add

static value_op_t sqlAddImpl(const Integer & lhs, const Integer & rhs, SqlType resultType)
{
    llvm::Value * raw = CreateOverflowSAdd(
        lhs.getLLVMValue(), rhs.getLLVMValue(), resultType, &genOverflowException);
    return value_op_t( Integer::fromRawValues({ raw }) );
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
        lhsRaw = CreateOverflowSMul(lhsRaw, shift.llvmValue, resultType, &genOverflowException);
    } else if (diff >  0) {
        cg_value_t shift( numericShifts[ diff ] );
        rhsRaw = CreateOverflowSMul(rhsRaw, shift.llvmValue, resultType, &genOverflowException);
    }

    // performe the arithmetic operation
    llvm::Value * raw = CreateOverflowSAdd(lhsRaw, rhsRaw, resultType, &genOverflowException);
    return value_op_t( Numeric::fromRawValues({ raw }, resultType) );
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
        } else if (lhs.type.typeID == TypeID::NumericID && rhs.type.typeID == TypeID::NumericID) {
            return sqlAddImpl(static_cast<const Numeric &>(lhs), static_cast<const Numeric &>(rhs), notNullableType);
        } else {
            throw InvalidOperationException("attempt to add incompatible types");
        }
    };
    return Utils::nullHandler(action, outerLhs, outerRhs, resultType);
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

    if (lhs.typeID == TypeID::IntegerID && rhs.typeID == TypeID::IntegerID) {
        result = lhs;
    } else if (lhs.typeID == TypeID::IntegerID && rhs.typeID == TypeID::NumericID) {
        result = rhs;
    } else if (lhs.typeID == TypeID::NumericID && rhs.typeID == TypeID::IntegerID) {
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
        lhs.getLLVMValue(), rhs.getLLVMValue(), resultType, &genOverflowException);
    return value_op_t( Integer::fromRawValues({ raw }) );
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
        lhsRaw = CreateOverflowSMul(lhsRaw, shift.llvmValue, resultType, &genOverflowException);
    } else if (diff >  0) {
        cg_value_t shift( numericShifts[ diff ] );
        rhsRaw = CreateOverflowSMul(rhsRaw, shift.llvmValue, resultType, &genOverflowException);
    }

    // performe the arithmetic operation
    llvm::Value * raw = CreateOverflowSSub(lhsRaw, rhsRaw, resultType, &genOverflowException);
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
        } else if (lhs.type.typeID == TypeID::NumericID && rhs.type.typeID == TypeID::NumericID) {
            return sqlSubImpl(static_cast<const Numeric &>(lhs), static_cast<const Numeric &>(rhs), notNullableType);
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
        lhs.getLLVMValue(), rhs.getLLVMValue(), resultType, &genOverflowException);
    return value_op_t( Integer::fromRawValues({ raw }) );
}

static value_op_t sqlMulImpl(const Numeric & lhs, const Numeric & rhs, SqlType resultType)
{
    llvm::Value * lhsRaw = lhs.getLLVMValue();
    llvm::Value * rhsRaw = rhs.getLLVMValue();

    // performe the arithmetic operation
    llvm::Value * raw = CreateOverflowSMul(lhsRaw, rhsRaw, resultType, &genOverflowException);
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
        } else if (lhs.type.typeID == TypeID::NumericID && rhs.type.typeID == TypeID::NumericID) {
            return sqlMulImpl(static_cast<const Numeric &>(lhs), static_cast<const Numeric &>(rhs), notNullableType);
        } else {
            throw InvalidOperationException("attempt to multiply incompatible types");
        }
    };
    return Utils::nullHandler(action, outerLhs, outerRhs, resultType);
}

bool inferMulTy(SqlType lhs, SqlType rhs, SqlType & result)
{
    bool valid = true;

    if (lhs.typeID == TypeID::IntegerID && rhs.typeID == TypeID::IntegerID) {
        result = lhs;
    } else if (lhs.typeID == TypeID::IntegerID && rhs.typeID == TypeID::NumericID) {
        result = rhs;
    } else if (lhs.typeID == TypeID::NumericID && rhs.typeID == TypeID::IntegerID) {
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

static value_op_t sqlDivImpl(const Integer & lhs, const Integer & rhs, SqlType resultType)
{
    auto & codeGen = getThreadLocalCodeGen();
    llvm::Value * value = codeGen->CreateSDiv(lhs.getLLVMValue(), rhs.getLLVMValue());
    return value_op_t( Integer::fromRawValues({ value }) );
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
        lhsRaw = CreateOverflowSMul(lhsRaw, shift.llvmValue, resultType, &genOverflowException);
    } else if (diff >  0) {
        if (diff > rhs.type.numericLayout.precision) {
            cg_value_t shift( numericShifts[ diff - rhs.type.numericLayout.precision ] );
            rhsRaw = CreateOverflowSMul(rhsRaw, shift.llvmValue, resultType, &genOverflowException);
        } else if (diff < rhs.type.numericLayout.precision) {
            cg_value_t shift( numericShifts[ rhs.type.numericLayout.precision - diff] );
            lhsRaw = CreateOverflowSMul(lhsRaw, shift.llvmValue, resultType, &genOverflowException);
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
        llvm::Value * zero = codeGen->getIntN( raw->getType()->getPrimitiveSizeInBits(), 0 );
        cg_bool_t isZero( codeGen->CreateICmpEQ(raw, zero) );

        IfGen zeroCheck(isZero);
        {
            Functions::genPrintfCall("division by zero\n");
            codeGen->CreateRetVoid();
        }
        zeroCheck.EndIf();

        // only values with the same type can be added
        if (lhs.type.typeID == TypeID::IntegerID && rhs.type.typeID == TypeID::IntegerID) {
            return sqlDivImpl(static_cast<const Integer &>(lhs), static_cast<const Integer &>(rhs), notNullableType);
        } else if (lhs.type.typeID == TypeID::NumericID && rhs.type.typeID == TypeID::NumericID) {
            return sqlDivImpl(static_cast<const Numeric &>(lhs), static_cast<const Numeric &>(rhs), notNullableType);
        } else {
            throw InvalidOperationException("attempt to divide incompatible types");
        }
    };
    return Utils::nullHandler(action, outerLhs, outerRhs, resultType);
}

bool inferDivTy(SqlType lhs, SqlType rhs, SqlType & result)
{
    bool valid = true;

    if (lhs.typeID == TypeID::IntegerID && rhs.typeID == TypeID::IntegerID) {
        result = lhs;
    } else if (lhs.typeID == TypeID::IntegerID && rhs.typeID == TypeID::NumericID) {
        result = rhs;
    } else if (lhs.typeID == TypeID::NumericID && rhs.typeID == TypeID::IntegerID) {
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
// equals

value_op_t sqlEquals(const Value & outerLhs, const Value & outerRhs)
{
    assert(Sql::equals(outerLhs.type, outerRhs.type, SqlTypeEqualsMode::WithoutNullable));

    SqlType resultType;
    bool valid = inferEqualsTy(outerLhs.type, outerRhs.type, resultType);
    assert(valid);

    auto action = [](const Value & lhs, const Value & rhs) {
        cg_bool_t result = lhs.equals(rhs);

        // convert to a SQL Bool
        return value_op_t( Bool::fromRawValues({ result }) );
    };
    return Utils::nullHandler(action, outerLhs, outerRhs, resultType);
}

bool inferEqualsTy(SqlType lhs, SqlType rhs, SqlType & result)
{
    return inferCompareTy(lhs, rhs, result);
}

//-----------------------------------------------------------------------------
// sqlIsNull

std::unique_ptr<Sql::Bool> sqlIsNull(const Value & value)
{
    throw NotImplementedException("sqlIsNull");
}

//-----------------------------------------------------------------------------
// casts

value_op_t castInteger(const Integer & value, SqlType newType)
{
    auto & codeGen = getThreadLocalCodeGen();

    // only Integer <-> Numeric conversions for now
    switch (newType.typeID) {
        case TypeID::IntegerID:
            return value.clone(); // identity
        case TypeID::NumericID: {
            llvm::Value * raw = value.getLLVMValue();
            raw = codeGen->CreateSExt(raw, Numeric::cg_value_type::getType());
            Numeric::cg_value_type shift( numericShifts[newType.numericLayout.precision] );
            raw = codeGen->CreateMul(raw, shift);
            return Numeric::fromRawValues({ raw }, newType);
        }
        default:
            throw ArithmeticException("no known conversion");
    }
}

value_op_t castNumeric(const Numeric & value, SqlType newType)
{
    auto & codeGen = getThreadLocalCodeGen();

    // only Integer <-> Numeric conversions for now
    switch (newType.typeID) {
        case TypeID::IntegerID: {
            llvm::Value * raw = value.getLLVMValue();
            Numeric::cg_value_type shift( numericShifts[newType.numericLayout.precision] );
            raw = codeGen->CreateSDiv(raw, shift);
            raw = codeGen->CreateTrunc(raw, Integer::cg_value_type::getType());
            return Integer::fromRawValues({ raw });
        }
        case TypeID::NumericID: {
            llvm::Value * raw = value.getLLVMValue();

            // adjust precision
            if (value.type.numericLayout.precision > newType.numericLayout.precision) {
                uint8_t diff = value.type.numericLayout.precision - newType.numericLayout.precision;
                Numeric::cg_value_type shift( numericShifts[diff] );
                raw = codeGen->CreateSDiv(raw, shift);
                return Numeric::fromRawValues({ raw }, newType);
            } else if (value.type.numericLayout.precision < newType.numericLayout.precision) {
                uint8_t diff = newType.numericLayout.precision - value.type.numericLayout.precision;
                Numeric::cg_value_type shift( numericShifts[diff] );
                raw = CreateOverflowSMul(raw, shift, newType, &genOverflowException);
                return Numeric::fromRawValues({ raw }, newType);
            } else {
                return value.clone();
            }
        }
        default:
            throw ArithmeticException("no known conversion");
    }
}

value_op_t sqlCast(const Value & outerValue, SqlType newType)
{
    assert(outerValue.type.nullable == newType.nullable);

    if (outerValue.type.typeID == Sql::SqlType::TypeID::UnknownID) {
        throw NotImplementedException("sqlCast() cast unknown");
    }

    SqlType castType = Sql::toNotNullableTy(newType);

    auto action = [&](const Value & value) {
        switch (value.type.typeID) {
            case TypeID::IntegerID:
                return castInteger(static_cast<const Integer &>(value), castType);
            case TypeID::NumericID:
                return castNumeric(static_cast<const Numeric &>(value), castType);
            default:
                throw ArithmeticException("no known conversion");
        }
    };

    return Utils::nullHandler(action, outerValue, castType);
}

//-----------------------------------------------------------------------------
// logical and

value_op_t sqlAnd(const Value & lhs, const Value & rhs)
{
    assert(lhs.type.typeID == SqlType::TypeID::BoolID);
    assert(rhs.type.typeID == SqlType::TypeID::BoolID);

    if (!isNullable(lhs) && !isNullable(rhs)) {
        cg_bool_t lhsRaw(lhs.getLLVMValue());
        cg_bool_t rhsRaw(rhs.getLLVMValue());
        cg_bool_t result = lhsRaw && rhsRaw;
        return Sql::Bool::fromRawValues({result.llvmValue});
    } else {
        Sql::Bool trueValue(true);
        auto nullableTrueValue = Sql::NullableValue::create(trueValue, cg_bool_t(false));
        PhiNode<Sql::Value> resultNode(*nullableTrueValue, "resultNode");

        IfGen cases(Utils::isFalse(lhs) || Utils::isFalse(rhs)); // any false -> false
        {
            Sql::Bool falseValue(false);
            auto nullableFalseValue = Sql::NullableValue::create(falseValue, cg_bool_t(false));
            resultNode.addIncoming(*nullableFalseValue);
        }
        cases.ElseIf(Utils::isNull(lhs) || Utils::isNull(rhs)); // any null -> null
        {
            auto nullValue = Sql::NullableValue::getNull(getBoolTy(true));
            resultNode.addIncoming(*nullValue);
        }
        cases.EndIf();

        return resultNode.get();
    }
}

value_op_t sqlOr(const Value & lhs, const Value & rhs)
{
    assert(lhs.type.typeID == SqlType::TypeID::BoolID);
    assert(rhs.type.typeID == SqlType::TypeID::BoolID);

    if (!isNullable(lhs) && !isNullable(rhs)) {
        cg_bool_t lhsRaw(lhs.getLLVMValue());
        cg_bool_t rhsRaw(rhs.getLLVMValue());
        cg_bool_t result = lhsRaw || rhsRaw;
        return Sql::Bool::fromRawValues({result.llvmValue});
    } else {
        Sql::Bool falseValue(false);
        auto nullableFalseValue = Sql::NullableValue::create(falseValue, cg_bool_t(false));
        PhiNode<Sql::Value> resultNode(*nullableFalseValue, "resultNode");

        IfGen cases(Utils::isTrue(lhs) || Utils::isTrue(rhs)); // any true -> true
        {
            Sql::Bool trueValue(true);
            auto nullableTrueValue = Sql::NullableValue::create(trueValue, cg_bool_t(false));
            resultNode.addIncoming(*nullableTrueValue);
        }
        cases.ElseIf(Utils::isNull(lhs) || Utils::isNull(rhs)); // any null -> null
        {
            auto nullValue = Sql::NullableValue::getNull(getBoolTy(true));
            resultNode.addIncoming(*nullValue);
        }
        cases.EndIf();

        return resultNode.get();
    }
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

} // end namespace Operators
} // end namespace Sql
