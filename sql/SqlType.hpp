
#ifndef SQLTYPE_HPP
#define SQLTYPE_HPP

#include <cstdint>

#include <llvm/IR/Type.h>

namespace Sql {

struct SqlType {
    // data:
    enum class TypeID : uint8_t {
        UnknownID, BoolID, IntegerID, LongIntegerID, CharID, VarcharID, NumericID, DateID, TimestampID, TextID,
        DoubleID, ArrayID
    } typeID;
    union {
        // char/varchar specific:
        uint32_t length;
        // numeric specific:
        struct {
            uint8_t length;
            uint8_t precision;
        } __attribute__ ((packed)) numericLayout;
    };

    bool nullable;

    // array specific: padding[0] is the TypeID of the elements, whose length (or numeric layout) is the one above
    uint8_t padding[2] = { };

    // constructors:

    SqlType() :
            typeID(TypeID::UnknownID), length(0), nullable(true)
    { }

    SqlType(TypeID _typeID, bool _nullable) :
            typeID(_typeID), length(0), nullable(_nullable)
    { }

} __attribute__ ((packed));

static_assert(sizeof(SqlType) == 8, "SqlType isn't packed.");

SqlType getUnknownTy();

SqlType getBoolTy(bool nullable = false);

SqlType getDateTy(bool nullable = false);

SqlType getIntegerTy(bool nullable = false);

SqlType getLongIntegerTy(bool nullable = false);

SqlType getNumericFullLengthTy(uint8_t precision, bool nullable = false);

SqlType getNumericTy(uint8_t length, uint8_t precision, bool nullable = false);

SqlType getCharTy(uint32_t length, bool nullable = false);

SqlType getVarcharTy(uint32_t capacity, bool nullable = false);

SqlType getTimestampTy(bool nullable = false);

SqlType getTextTy(bool nullable = false);

SqlType getDoubleTy(bool nullable = false);

/// \brief An array of the given elements (of any number of dimensions, as in PostgreSQL)
SqlType getArrayTy(SqlType elementType, bool nullable = false);

/// \returns The type of the elements of an array, which may be NULL
SqlType getElementTy(SqlType arrayType);

/// \brief calculate the storage size
size_t getValueSize(SqlType type);

std::string getName(SqlType type);

SqlType toNullableTy(SqlType type);

SqlType toNotNullableTy(SqlType type);

llvm::Type * toLLVMTy(SqlType type); // implements caching

enum class SqlTypeEqualsMode { Full, WithoutNullable };

bool equals(SqlType type1, SqlType type2, SqlTypeEqualsMode mode);

bool sameTypeID(SqlType type1, SqlType type2);

} // end namespace Sql

#endif // SQLTYPE_HPP
