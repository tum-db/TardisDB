
#pragma once

#include <memory>

#include "logical/operators.hpp"
#include "physical/Operator.hpp"

namespace Algebra {

/// \note This function must only be used inside a code generation context.
/// This means that there already has to be a valid ModuleGen available within the current context.
std::unique_ptr<Physical::Operator> translateToPhysicalTree(const Logical::Operator & resultOperator);

} // end namespace Algebra
