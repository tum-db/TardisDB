#ifndef PROTODB_IUFACTORY_HPP
#define PROTODB_IUFACTORY_HPP

#include <cstdint>
#include <memory>
#include <set>
#include <unordered_map>

#include "foundations/Database.hpp"
#include "semanticAnalyser/logicalAlgebra/InformationUnit.hpp"
#include "sql/SqlType.hpp"

class IUFactory {
public:
    IUFactory() = default;

    /// \brief Create an iu for a temporary
    iu_p_t createIU(Sql::SqlType type);

    /// \brief Create an iu for a given column
    iu_p_t createIU(const uint32_t operatorUID, ci_p_t columnInformation);

private:
    using iu_op_t = std::unique_ptr<InformationUnit>;

    std::vector<iu_op_t> iu_vec;
};

#endif // PROTODB_IUFACTORY_HPP