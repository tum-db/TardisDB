#include "foundations/IUFactory.hpp"

iu_p_t IUFactory::createIU(Sql::SqlType type)
{
    iu_op_t iu = std::make_unique<InformationUnit>();
    iu->iuType = InformationUnit::Type::ValueRef;
    iu->sqlType = type;
    iu_vec.push_back(std::move(iu));
    return iu_vec.back().get();
}

iu_p_t IUFactory::createIU(const uint32_t operatorUID, ci_p_t columnInformation)
{
    iu_op_t iu = std::make_unique<InformationUnit>();
    iu->iuType = InformationUnit::Type::ColumnRef;
    iu->sqlType = columnInformation->type;
    iu->columnInformation = columnInformation;
    iu->scanUID = operatorUID;
    iu_vec.push_back(std::move(iu));
    return iu_vec.back().get();
}
