
#ifndef SQLTYPE_HPP
#define SQLTYPE_HPP

#include <cstdint>

#include <llvm/IR/Type.h>

namespace Sql {

struct SqlType {
    // data:
    enum class TypeID : uint8_t {
        UnknownID, BoolID, IntegerID, CharID, VarcharID, NumericID, DateID, TimestampID
    } typeID;
    union {
        uint32_t length;
        struct {
            uint8_t length;
            uint8_t precision;
        } __attribute__ ((packed)) numericLayout;
    };

    bool nullable;

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

SqlType getNumericFullLengthTy(uint8_t precision, bool nullable = false);

SqlType getNumericTy(uint8_t length, uint8_t precision, bool nullable = false);

SqlType getCharTy(uint32_t length, bool nullable = false);

SqlType getVarcharTy(uint32_t capacity, bool nullable = false);

SqlType getTimestampTy(bool nullable = false);

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
