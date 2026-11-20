#pragma once

#include <array>

#include "algebra/physical/Operator.hpp"
#include "algebra/physical/expressions.hpp"
#include "foundations/Database.hpp"
#include "foundations/version_management.hpp"
#include "sql/SqlValues.hpp"

namespace Algebra {
namespace Physical {

/// Aggregation per branch over a set of branches:
/// SELECT branch, sum(x) FROM t VERSION ( b1 , b2 ) GROUP BY branch
///
/// Shared strategy (interval visibility only): one pass over the table. A tuple whose in-place
/// version heads its chain, lies in the bitmaps of all branches and carries a label containing the
/// labels of all readers is aggregated once for all of them. For any other tuple,
/// get_visible_ranges() walks the chain once and splits the readers, ordered by label, into ranges
/// that see the same version; count and sum go to a difference array over the readers, min and max
/// to every reader of the range.
///
/// PerBranch strategy: one scan per branch, with the same visibility test as TableScan.
///
/// Both strategies run morsel-driven on Parallel::threads threads: a compiled
/// morsel function scans a range of tuples (for one branch, in the PerBranch strategy) and
/// aggregates into the groups of its worker; the groups of all workers are merged at the end.
class BranchAggregation : public NullaryOperator {
public:
    BranchAggregation(const logical_operator_t & logicalOperator, std::vector<Expressions::exp_op_t> predicates, QueryContext & queryContext);

    ~BranchAggregation() override;

    void produce() override;

    /// Runtime state; owned by the execution context since the compiled query outlives this operator
    struct State : public ExecutionResource {
        BranchSet set;
        size_t width;                       // slots per group: count, one per aggregate, one per aggregate of a nullable column
        std::vector<int> functions;         // Logical::BranchAggregation::Function per aggregate
        std::vector<size_t> nonNull;        // per aggregate, the slot that counts its values that are not NULL; 0 (the count) if it has no NULLs
        size_t workers;
        size_t tuples;
        bool perBranch;
        // per worker, apart by at least a cache line: (k + 1) groups each, groupStride slots apart
        size_t groupStride;
        std::vector<int64_t> direct;        // groups of the k readers, then the group read by all
        std::vector<int64_t> diff;          // difference array of count and sum over the k readers
        std::vector<int64_t> result;        // final groups in the order of the query
        std::vector<uint32_t> position;     // position of the j-th branch of the query
        // per worker, apart by at least a cache line: k entries each, scratchStride entries apart;
        // output of get_visible_ranges
        size_t scratchStride;
        std::vector<const void *> ptrs;
        std::vector<uint32_t> lo;
        std::vector<uint32_t> hi;
        std::vector<const std::unordered_map<branch_id_t, branch_id_t> *> lineages; // per position, set at run time
        std::vector<uint32_t> minLimits;    // COMPILED_LINEAGE: per writer, the least limit of the readers
        std::vector<std::array<uint64_t, 2>> names; // the names of the branches in the order of the query, as SQL texts
    };

private:
    using column_t = std::tuple<iu_p_t, ci_p_t, size_t, llvm::Type *, llvm::Value *>; // iu, ci, index, column type, column pointer

    /// \returns The function void morsel(begin, end, worker, position)
    llvm::Function * genMorselFunction(State & state);
    void genSharedScan(State & state, llvm::Value * begin, llvm::Value * end);
    void genPerBranchScan(State & state, llvm::Value * begin, llvm::Value * end);
    void genOutput(State & state);

    /// \returns The address of the first element of the worker's part of a per-worker array
    template<typename T>
    llvm::Value * genWorkerBase(std::vector<T> & array, size_t perWorker, llvm::Type * elementTy);

    /// \returns Whether the bitmap of the branch (a runtime value) contains the tuple
    llvm::Value * genIsInBranch(cg_tid_t tid, llvm::Value * branch);

    /// \returns (all readers contain the tuple, any reader contains it)
    std::pair<llvm::Value *, llvm::Value *> genPresence(State & state, cg_tid_t tid);

    /// Loads the needed columns from the in-place version (tuplePtr == nullptr) or the given tuple
    iu_value_mapping_t genLoad(cg_tid_t tid, llvm::Value * tuplePtr);

    /// Generates "if all predicates hold"; to be closed by the caller
    llvm::Value * genPredicate(const iu_value_mapping_t & values);

    llvm::Value * genInputValue(const iu_value_mapping_t & values, size_t aggregate);
    /// \returns Whether the input of the aggregate is not NULL (i1), or nullptr if it has no NULLs
    llvm::Value * genInputPresent(const iu_value_mapping_t & values, size_t aggregate);

    void genAccumulate(State & state, llvm::Value * group, const iu_value_mapping_t & values);
    void genAccumulateRange(State & state, llvm::Value * lo, llvm::Value * hi, const iu_value_mapping_t & values);

    llvm::Value * genSlotPtr(llvm::Value * base, llvm::Value * group, size_t width, size_t slot);

    Table & _table;
    std::vector<Expressions::exp_op_t> _predicates;
    std::vector<column_t> _columns;
    std::vector<Sql::value_op_t> _loaded;

    // inside the morsel function
    llvm::Value * _worker = nullptr;
    llvm::Value * _position = nullptr;
    llvm::Value * _direct = nullptr;
    llvm::Value * _diff = nullptr;
};

} // end namespace Physical
} // end namespace Algebra
