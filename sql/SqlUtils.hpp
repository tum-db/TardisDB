
#pragma once

#include <unordered_map>

#include "codegen/CodeGen.hpp"
#include "codegen/PhiNode.hpp"
#include "foundations/exceptions.hpp"
#include "foundations/utils.hpp"
#include "sql/SqlValues.hpp"

namespace Sql {
namespace Utils {

/// \returns True if the given boolean SQL value is true
cg_bool_t isTrue(const Value & value);

/// \returns True if the given boolean SQL value is true
cg_bool_t isFalse(const Value & value);

/// \returns True if the given boolean SQL value is null
cg_bool_t isNull(const Value & value);

/// \brief Prints the given SQL value to stdout
void genPrintValue(Value & sqlValue);

/// \brief Performs different actions depending on whether the given SQL value is null
template<typename F>
auto nullHandlerExtended(F presentFunc, F absentFunc, const Value & value) -> decltype(presentFunc(value));

/// \brief Performs the given action if the given SQL value is not null
template<typename ActionFun>
value_op_t nullHandler(ActionFun action, const Value & a);

/// \brief Performs the given action if non given SQL values is null
template<typename ActionFun>
value_op_t nullHandler(ActionFun action, const Value & a, const Value & b);

/// \brief Create a nullable wrapper SQL value
inline value_op_t createNullValue(value_op_t value);

/// \brief LLVM's select statement for SQL values
value_op_t genSelect(llvm::Value * condition, const Sql::Value & trueValue, const Sql::Value & falseValue);

} // end namespace Utils
} // end namespace Sql

#include "SqlUtils.tcc"
