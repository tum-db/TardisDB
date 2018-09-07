
#pragma once

#include <cstdint>
#include <memory>
#include <set>
#include <unordered_map>

#include "Database.hpp"
#include "sql/SqlType.hpp"
#include "utils.hpp"

namespace Algebra {
namespace Logical {
class TableScan;
}}

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

class IUFactory {
public:
    IUFactory() = default;

    /// \brief Create an iu for a temporary
    iu_p_t createIU(Sql::SqlType type);

    /// \brief Create an iu for a given column
    iu_p_t createIU(const Algebra::Logical::TableScan & producer, ci_p_t columnInformation);

private:
    using iu_op_t = std::unique_ptr<InformationUnit>;

    std::vector<iu_op_t> iu_vec;
};

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
