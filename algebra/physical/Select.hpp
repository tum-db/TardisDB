
#pragma once

#include "algebra/physical/Operator.hpp"
#include "algebra/physical/expressions.hpp"

namespace Algebra {
namespace Physical {

/// The selection operator
class Select : public UnaryOperator {
public:
    enum ArithmeticOperator { Equals };

    Select(std::unique_ptr<Operator> input, const iu_set_t & required, Expressions::exp_op_t exp);

    virtual ~Select();

    void produce() override;
    void consume(const iu_value_mapping_t & values, const Operator & src) override;

private:
    std::string constant;
    Expressions::exp_op_t _exp;
};

} // end namespace Physical
} // end namespace Algebra
