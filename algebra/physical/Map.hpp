 
#pragma once

#include "Operator.hpp"
#include "expressions.hpp"

namespace Algebra {
namespace Physical {

struct Mapping {
    iu_p_t source;
    iu_p_t dest;
    Expressions::exp_op_t exp;
};

/// The map operator
class Map : public UnaryOperator {
public:
    Map(std::unique_ptr<Operator> input, const iu_set_t & required, std::vector<Mapping> mappings);
    virtual ~Map();

    void produce() override;
    void consume(const iu_value_mapping_t & values, const Operator & src) override;

private:
    std::vector<Mapping> _mappings;
};

} // end namespace Physical
} // end namespace Algebra
