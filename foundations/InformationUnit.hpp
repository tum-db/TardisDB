#ifndef PROTODB_INFORMATIONUNIT_HPP
#define PROTODB_INFORMATIONUNIT_HPP

#include <cstdint>
#include <memory>
#include <set>
#include <unordered_map>

#include "foundations/Database.hpp"
#include "sql/SqlType.hpp"

struct InformationUnit {
    enum class Type { ColumnRef, ValueRef } iuType;
    Sql::SqlType sqlType;
    ci_p_t columnInformation; // only valid if iuType == ColumnRef
    uint32_t scanUID; // only valid if iuType == ColumnRef

    InformationUnit() { }
    InformationUnit(const InformationUnit &) = delete;
};

using iu_p_t = const InformationUnit *;
using iu_set_t = std::set<iu_p_t>;

inline Sql::SqlType getType(iu_p_t iu)
{
    return iu->sqlType;
}

inline ci_p_t getColumnInformation(iu_p_t iu)
{
    assert(iu->iuType == InformationUnit::Type::ColumnRef);
    return iu->columnInformation;
}

inline bool isValid(iu_p_t iu)
{
    return (iu != nullptr);
}

#endif // PROTODB_INFORMATIONUNIT_HPP