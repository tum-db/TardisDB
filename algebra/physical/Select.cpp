#include "algebra/physical/Select.hpp"
#include "foundations/exceptions.hpp"
#include "sql/SqlValues.hpp"
#include "sql/SqlUtils.hpp"

using namespace Sql;

namespace Algebra {
namespace Physical {

Select::Select(const logical_operator_t & logicalOperator, std::unique_ptr<Operator> input, Expressions::exp_op_t exp) :
        UnaryOperator(std::move(logicalOperator), std::move(input)),
        _exp(std::move(exp))
{ }

Select::~Select()
{ }

void Select::produce()
{
    child->produce();
}

void Select::consume(const iu_value_mapping_t & values, const Operator & src)
{
    value_op_t match = _exp->evaluate(values);
    assert(match->type.typeID == Sql::SqlType::TypeID::BoolID);
    cg_bool_t result = Sql::Utils::isTrue(*match);

    // pass only matching tuples
    IfGen check(result);
    {
        _parent->consume(values, *this);
    }
    check.EndIf();
}

} // end namespace Physical
} // end namespace Algebra
