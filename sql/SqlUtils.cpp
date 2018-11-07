#include "sql/SqlUtils.hpp"

#include <llvm/IR/TypeBuilder.h>

#include "foundations/LegacyTypes.hpp"
#include "sql/SqlValues.hpp"
#include "utils/general.hpp"

namespace Sql {

using TypeID = SqlType::TypeID;

namespace Utils {

cg_bool_t isTrue(const Value & value)
{
    using actionType = std::function<llvm::Value * (const Value &)>;

    actionType presentAction = [](const Value & _value) {
        if (_value.type.typeID == TypeID::BoolID) {
            return _value.getLLVMValue();
        } else {
            throw InvalidOperationException("attempt to compare incompatible types");
        }
    };

    actionType absentAction = [](const Value & ) {
        return cg_bool_t(false).llvmValue;
    };

    auto result = Utils::nullHandlerExtended(presentAction, absentAction, value);
    return cg_bool_t(result);
}

cg_bool_t isFalse(const Value & value)
{
    using actionType = std::function<llvm::Value * (const Value &)>;

    actionType presentAction = [](const Value & _value) {
        if (_value.type.typeID == TypeID::BoolID) {
            return !cg_bool_t(_value.getLLVMValue());
        } else {
            throw InvalidOperationException("attempt to compare incompatible types");
        }
    };

    actionType absentAction = [](const Value & ) {
        return cg_bool_t(false).llvmValue;
    };

    auto result = Utils::nullHandlerExtended(presentAction, absentAction, value);
    return cg_bool_t(result);
}

cg_bool_t isNull(const Value & value)
{
    // null constant?
    if (value.type.typeID == SqlType::TypeID::UnknownID) {
        return cg_bool_t(true);
    }

    if (Sql::isNullable(value)) {
        const NullableValue & nullableValue = static_cast<const NullableValue &>(value);
        return nullableValue.isNull();
    } else {
        return cg_bool_t(false);
    }
}

static void genPrintNumericCall(Value & sqlValue)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();

    // void printNumeric(unsigned len, unsigned precision, int64_t raw)
    llvm::FunctionType * type = llvm::TypeBuilder<void(unsigned, unsigned, int64_t), false>::get(context);

    unsigned len = sqlValue.type.numericLayout.length;
    unsigned precision = sqlValue.type.numericLayout.precision;
    codeGen.CreateCall( &printNumeric, type, { cg_unsigned_t(len), cg_unsigned_t(precision), sqlValue.getLLVMValue() } );
}

static void genPrintDateCall(Value & sqlValue)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();

    // void printDate(int32_t raw);
    llvm::FunctionType * type = llvm::TypeBuilder<void(int32_t), false>::get(context);
    codeGen.CreateCall(&printDate, type, sqlValue.getLLVMValue());
}

static void genPrintTimestampCall(Value & sqlValue)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();

    // void printTimestamp(uint64_t raw);
    llvm::FunctionType * type = llvm::TypeBuilder<void(uint64_t), false>::get(context);
    codeGen.CreateCall(&printTimestamp, type, sqlValue.getLLVMValue());
}

static void genPrintAvailableValue(Value & sqlValue)
{
    using namespace Functions;

    switch (sqlValue.type.typeID) {
        case SqlType::TypeID::UnknownID:
            genPrintfCall("null");
            break;
        case SqlType::TypeID::BoolID: // fallthrough
        case SqlType::TypeID::IntegerID:
            genPrintfCall("%d", sqlValue.getLLVMValue());
            break;
        case SqlType::TypeID::VarcharID: {
            Varchar * varcharValue = static_cast<Varchar *>(&sqlValue);
            cg_size_t len(varcharValue->getLength());
            genPrintfCall("%.*s", len, sqlValue.getLLVMValue());
            break;
        }
        case SqlType::TypeID::CharID: {
            if (sqlValue.type.length == 1) {
                genPutcharCall(cg_int_t(sqlValue.getLLVMValue()));
            } else {
                Char * charValue = static_cast<Char *>(&sqlValue);
                cg_size_t len(charValue->getLength());
                genPrintfCall("%.*s", len, sqlValue.getLLVMValue());
            }
            break;
        }
        case SqlType::TypeID::NumericID:
            genPrintNumericCall(sqlValue);
            break;
        case SqlType::TypeID::DateID:
            genPrintDateCall(sqlValue);
            break;
        case SqlType::TypeID::TimestampID:
            genPrintTimestampCall(sqlValue);
            break;
        default:
            unreachable();
    }
}

void genPrintValue(Value & sqlValue)
{
    if (isNullable(sqlValue)) {
        const NullableValue & nullableValue = static_cast<const NullableValue &>(sqlValue);
        cg_bool_t isNull = nullableValue.isNull();

        IfGen nullCheck(isNull);
        {
            Functions::genPrintfCall("null");
        }
        nullCheck.Else();
        {
            genPrintAvailableValue(sqlValue);
        }
        nullCheck.EndIf();
    } else {
        genPrintAvailableValue(sqlValue);
    }
}

value_op_t genSelect(llvm::Value * condition, const Sql::Value & trueValue, const Sql::Value & falseValue)
{
    assert(Sql::equals(trueValue.type, falseValue.type, Sql::SqlTypeEqualsMode::Full));

    auto & codeGen = getThreadLocalCodeGen();
    auto trueRawValues = trueValue.getRawValues();
    auto falseRawValues = falseValue.getRawValues();

    std::vector<llvm::Value *> selectedValues;
    for (unsigned i = 0, size = trueRawValues.size(); i < size; ++i) {
        llvm::Value * selected = codeGen->CreateSelect(condition, trueRawValues[i], falseRawValues[i]);
        selectedValues.push_back(selected);
    }

    return Sql::Value::fromRawValues(selectedValues, trueValue.type);
}

} // end namespace Utils
} // end namespace Sql
