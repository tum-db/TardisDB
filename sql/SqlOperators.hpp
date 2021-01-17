
#pragma once

#include "sql/SqlType.hpp"
#include "sql/SqlValues.hpp"

namespace Sql {
namespace Operators {

std::unique_ptr<Sql::Bool> sqlIsNull(const Value & value);

value_op_t sqlCast(const Value & value, SqlType newType);

/// \returns A boolean SQL Value
value_op_t sqlCompare(const Value & lhs, const Value & rhs, ComparisonMode mode);

/// \param result [out]
/// \result True if valid
bool inferCompareTy(SqlType lhs, SqlType rhs, SqlType & result);

/// \returns A boolean SQL Value
value_op_t sqlEquals(const Value & lhs, const Value & rhs);

/// \param result [out]
/// \result True if valid
bool inferEqualsTy(SqlType lhs, SqlType rhs, SqlType & result);

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

    static bool overflowFlag = false;

    void genOverflowException();

    void genOverflowEvaluation();

    cg_bool_t genEvaluateOverflow();

} // end namespace Operators
} // end namespace Sql
