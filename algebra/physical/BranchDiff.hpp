#pragma once

#include <array>

#include "algebra/physical/Operator.hpp"
#include "algebra/physical/expressions.hpp"
#include "foundations/Database.hpp"
#include "foundations/version_management.hpp"
#include "sql/SqlValues.hpp"

namespace Algebra {
namespace Physical {

/// The tuples of one table that differ between two branches:
/// SELECT * FROM DIFF ( t VERSION a , t VERSION b )
///
/// At run time, diff_branches() compares per tuple the bitmaps and the visible versions of both
/// branches; a tuple seen in only one branch, or with different values, is produced once per branch
/// that sees it, together with the name of that branch.
class BranchDiff : public NullaryOperator {
public:
    BranchDiff(const logical_operator_t & logicalOperator, std::vector<Expressions::exp_op_t> predicates, QueryContext & queryContext);

    ~BranchDiff() override;

    void produce() override;

    /// Runtime state; owned by the execution context since the compiled query outlives this operator
    struct State : public ExecutionResource {
        Table * table;
        branch_id_t branches[2];
        std::vector<DiffEntry> entries;
        std::array<std::array<uint64_t, 2>, 2> names; // the names of both branches as SQL texts
    };

private:
    using column_t = std::tuple<iu_p_t, ci_p_t, size_t, llvm::Type *, llvm::Value *>; // iu, ci, index, column type, column pointer

    /// Loads the columns from the in-place version (tuplePtr is null) or the given tuple
    iu_value_mapping_t genLoad(cg_tid_t tid, llvm::Value * tuplePtr);

    Table & _table;
    std::vector<Expressions::exp_op_t> _predicates;
    std::vector<column_t> _columns;
    std::vector<Sql::value_op_t> _loaded;
};

} // end namespace Physical
} // end namespace Algebra
