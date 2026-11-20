#include "algebra/physical/Map.hpp"

namespace Algebra {
namespace Physical {

Map::Map(const logical_operator_t & logicalOperator, std::unique_ptr<Operator> input, std::vector<Mapping> mappings, QueryContext & queryContext) :
        UnaryOperator(std::move(logicalOperator), std::move(input), queryContext),
        _mappings(std::move(mappings))
{ }

Map::~Map()
{ }

void Map::produce()
{
    child->produce();
}

void Map::consume(const iu_value_mapping_t & values, const Operator & src)
{
    // the computed values live until the parent consumed the tuple
    iu_value_mapping_t extended = values;
    std::vector<Sql::value_op_t> computed;
    for (auto & mapping : _mappings) {
        computed.push_back(mapping.exp->evaluate(values));
        extended[mapping.dest] = computed.back().get();
    }
    _parent->consume(extended, *this);
}

} // end namespace Physical
} // end namespace Algebra
