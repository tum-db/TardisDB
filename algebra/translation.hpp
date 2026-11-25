
#pragma once

#include <memory>

#include "logical/operators.hpp"
#include "physical/Operator.hpp"

namespace Algebra {

/// The join of the translation: the hash join, or the sort-merge join (as PostgreSQL's SET enable_hashjoin =
/// off), which takes the joins whose keys it can order and leaves the others to the hash join
enum class JoinMethod { Hash, Merge };

/// \returns The join method of the translations of the current thread (a session of pgserver has its thread)
JoinMethod & joinMethod();

/// \note This function must only be used inside a code generation context.
/// This means that there already has to be a valid ModuleGen available within the current context.
std::unique_ptr<Physical::Operator> translateToPhysicalTree(const Logical::Operator & resultOperator, QueryContext &queryContext);

} // end namespace Algebra
