#pragma once

#include <cstdint>
#include <memory>
#include <set>
#include <unordered_map>

#include "foundations/Database.hpp"
#include "semanticAnalyser/InformationUnit.hpp"
#include "sql/SqlType.hpp"

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
