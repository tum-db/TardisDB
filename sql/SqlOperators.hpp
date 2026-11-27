
#pragma once

#include "sql/SqlType.hpp"
#include "sql/SqlValues.hpp"

namespace Sql {
namespace Operators {

/// An explicit cast (see Casts::cast)
value_op_t sqlCast(const Value & value, SqlType newType);

/// lhs * rhs of two integers of the same width; an overflow sets the overflow flag
llvm::Value * genOverflowMul(llvm::Value * lhs, llvm::Value * rhs);

/// \returns A boolean SQL Value
value_op_t sqlCompare(const Value & lhs, const Value & rhs, ComparisonMode mode);

/// \param result [out]
/// \result True if valid
bool inferCompareTy(SqlType lhs, SqlType rhs, SqlType & result);

value_op_t sqlAdd(const Value & lhs, const Value & rhs);

/// \param result [out]
/// \result True if valid
bool inferAddTy(SqlType lhs, SqlType rhs, SqlType & result);

value_op_t sqlSub(const Value & lhs, const Value & rhs);

/// \param result [out]
/// \result True if valid
bool inferSubTy(SqlType lhs, SqlType rhs, SqlType & result);

value_op_t sqlMul(const Value & lhs, const Value & rhs);

/// \param result [out]
/// \result True if valid
bool inferMulTy(SqlType lhs, SqlType rhs, SqlType & result);

value_op_t sqlDiv(const Value & lhs, const Value & rhs);

/// \param result [out]
/// \result True if valid
bool inferDivTy(SqlType lhs, SqlType rhs, SqlType & result);

value_op_t sqlAnd(const Value & lhs, const Value & rhs);

value_op_t sqlOr(const Value & lhs, const Value & rhs);

value_op_t sqlNot(const Value & value);

    /// Ends the statement with the error \p message: a call of the runtime, which throws (an exception of the generated code)
    void genRuntimeError(const std::string & message);

} // end namespace Operators
} // end namespace Sql
