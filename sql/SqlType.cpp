#include "sql/SqlType.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "codegen/CodeGen.hpp"
#include "foundations/exceptions.hpp"
#include "sql/SqlValues.hpp"

namespace Sql {

using TypeID = SqlType::TypeID;

SqlType getUnknownTy()
{
    static SqlType type;
    assert(type.typeID == TypeID::UnknownID);
    return type;
}

SqlType getBoolTy(bool nullable)
{
    SqlType type(TypeID::BoolID, nullable);
    return type;
}

SqlType getDateTy(bool nullable)
{
    SqlType type(TypeID::DateID, nullable);
    return type;
}

SqlType getIntegerTy(bool nullable)
{
    SqlType type(TypeID::IntegerID, nullable);
    return type;
}

SqlType getNumericFullLengthTy(uint8_t precision, bool nullable)
{
    static constexpr int maxDigits = std::numeric_limits<long>::digits10;
    return getNumericTy(maxDigits, precision, nullable);
}

SqlType getNumericTy(uint8_t length, uint8_t precision, bool nullable)
{
    static constexpr int maxDigits = std::numeric_limits<long>::digits10;
    // enforce some limitations
    length = std::min(length, static_cast<decltype(length)>(maxDigits));
    precision = std::min(precision, length);
    SqlType type(TypeID::NumericID, nullable);
    type.numericLayout.length = length;
    type.numericLayout.precision = precision;
    return type;
}

SqlType getCharTy(uint32_t length, bool nullable)
{
    SqlType type(TypeID::CharID, nullable);
    type.length = length;
    return type;
}

SqlType getVarcharTy(uint32_t capacity, bool nullable)
{
    SqlType type(TypeID::VarcharID, nullable);
    type.length = capacity;
    return type;
}

SqlType getTimestampTy(bool nullable)
{
    SqlType type(TypeID::TimestampID, nullable);
    return type;
}

SqlType getTextTy(bool nullable)
{
    SqlType type(TypeID::TextID, nullable);
    return type;
}

size_t getValueSize(SqlType type)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & dataLayout = codeGen.getCurrentModuleGen().getDataLayout();
    size_t size = dataLayout.getTypeAllocSize( toLLVMTy(type) );
    return size;
}

std::string getName(SqlType type)
{
    std::string signature;

    // construct the type signature
    switch (type.typeID) {
        case TypeID::UnknownID:
            signature = "Unknown";
            break;
        case TypeID::BoolID:
            signature = "Bool";
            break;
        case TypeID::IntegerID:
            signature = "Integer";
            break;
        case TypeID::CharID:
            signature = "Char<" + std::to_string(type.length) + ">";
            break;
        case TypeID::VarcharID:
            signature = "Varchar<" + std::to_string(type.length) + ">";
            break;
        case TypeID::NumericID:
            signature = "Numeric<" + std::to_string(type.numericLayout.length) + ", " +
                   std::to_string(type.numericLayout.precision) + ">";
            break;
        case TypeID::DateID:
            signature = "Date";
            break;
        case TypeID::TimestampID:
            signature = "Timestamp";
            break;
        case TypeID::TextID:
            signature = "Text";
            break;
        default:
            throw NotImplementedException("getName()");
    }

    // add the nullable marker
    if (type.nullable) {
        signature += "?";
    }

    return signature;
}

SqlType toNullableTy(SqlType type)
{
    type.nullable = true;
    return type;
}

SqlType toNotNullableTy(SqlType type)
{
    type.nullable = false;
    return type;
}

static llvm::Type * createLLVMTy(const SqlType & type, const std::string & name)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();

    // add the external null indicator
#ifndef USE_INTERNAL_NULL_INDICATOR
    if (type.nullable) {
        SqlType notNullType = type;
        notNullType.nullable = false;

        llvm::Type * valueTy = toLLVMTy(notNullType);
        std::string typeName = getName(type);
        llvm::StructType * nullableTy = llvm::StructType::create(context, typeName);
        nullableTy->setBody({ valueTy, codeGen->getInt1Ty() });

        return nullableTy;
    }
#endif

    switch (type.typeID) {
        case TypeID::UnknownID:
            return llvm::Type::getVoidTy(context);
        case TypeID::BoolID:
            return llvm::Type::getInt1Ty(context);
        case TypeID::IntegerID:
            return llvm::Type::getInt32Ty(context);
        case TypeID::CharID:
            if (type.length == 1) {
                return llvm::Type::getInt8Ty(context);
            }
            // fallthrough
        case TypeID::VarcharID: {
            unsigned lengthIndicatorSize =
                    (type.length < 256) ? 8 : (type.length < 65536) ? 16 : 32;

            llvm::StructType * structType = llvm::StructType::create(context, name);
            structType->setBody({
                    llvm::Type::getIntNTy(context, lengthIndicatorSize),
                    llvm::ArrayType::get(llvm::Type::getInt8Ty(context), type.length)
            });
            return structType;
        }
        case TypeID::NumericID:
            return llvm::Type::getInt64Ty(context);
        case TypeID::DateID:
            return llvm::Type::getInt32Ty(context);
        case TypeID::TimestampID:
            return llvm::Type::getInt64Ty(context);
        default:
            throw NotImplementedException("createLLVMTy()");
    }
}

llvm::Type * toLLVMTy(SqlType type)
{
    auto & typeCache = getThreadLocalCodeGen().getTypeCache();

    std::string name = getName(type);
    llvm::Type * llvmTy = typeCache.get(name); // lookup

    // create the type signature
    if (llvmTy == nullptr) {
        llvmTy = createLLVMTy(type, name);
        typeCache.add(name, llvmTy);
    }

    return llvmTy;
}

bool equals(SqlType type1, SqlType type2, SqlTypeEqualsMode mode)
{
    if (type1.typeID == SqlType::TypeID::TextID) {
        return (type2.typeID == SqlType::TypeID::TextID);
    }

    switch (mode) {
        case SqlTypeEqualsMode::WithoutNullable:
            type1.nullable = false;
            type2.nullable = false;
        default:
            return (std::memcmp(&type1, &type2, sizeof(SqlType)) == 0);
    }
}

bool sameTypeID(SqlType type1, SqlType type2)
{
    return (type1.typeID == type2.typeID);
}

} // end namespace Sql
