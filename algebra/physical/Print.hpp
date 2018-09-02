
#pragma once

#include "algebra/physical/Operator.hpp"

namespace Algebra {
namespace Physical {

/// The print operator
class Print : public UnaryOperator {
public:
    Print(std::unique_ptr<Operator> input, const std::vector<iu_p_t> & selection);

    virtual ~Print();

    void produce() override;
    void consume(const iu_value_mapping_t & values, const Operator & src) override;

private:
    std::vector<iu_p_t> selection;
    llvm::Value * tupleCountPtr;
};

} // end namespace Physical
} // end namespace Algebra
