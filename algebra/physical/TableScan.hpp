
#pragma once

#include "algebra/physical/Operator.hpp"
#include "foundations/Database.hpp"
#include "sql/SqlValues.hpp"

namespace Algebra {
namespace Physical {

/// The table scan operator
class TableScan : public NullaryOperator {
public:
    TableScan(const logical_operator_t & logicalOperator, Table & table, branch_id_t branchId, QueryContext &queryContext);

    virtual ~TableScan();

    void produce() override;

    void produce(cg_tid_t tid, branch_id_t branchId);

    /// What the workers of a parallel scan share; owned by the execution context
    struct ParallelState : public ExecutionResource {
        Table * table;
        branch_id_t branch;
        size_t tuples;
        uint64_t label = 0;                                                      // interval visibility
        const std::unordered_map<branch_id_t, branch_id_t> * lineage = nullptr;  // otherwise; set at run time
        std::vector<uint32_t> limits;                                            // COMPILED_LINEAGE
    };

    /// The lineageLimits() of the reader of a scan that does not run morsel-driven (COMPILED_LINEAGE)
    struct LimitsState : public ExecutionResource {
        std::vector<uint32_t> limits;
    };

    /// The candidates of an index probe; owned by the execution context
    struct ProbeState : public ExecutionResource {
        const BranchIndex * index;
        std::string key;
        std::vector<tid_t> candidates; // a copy: an UPDATE of the branch adds to the index while it scans
    };

    /// Produces the tuples of the tids (count of them at tids, an array of i64) that are visible in the
    /// branch of the scan; prepare() first, once per query
    void produceCandidates(llvm::Value * count, llvm::Value * tids);

    /// What a scan needs before it produces tuples: false if the table is empty
    bool prepare();

    /// \returns Whether the scan runs morsel-driven in the workers (its operators up to the result, or an aggregation)
    bool runsParallel();

private:
    using column_t = std::tuple<ci_p_t, llvm::Type *, llvm::Value *, size_t, Sql::value_op_t>;

    cg_voidptr_t genGetLatestEntryCall(cg_tid_t tid, branch_id_t branchId);
    llvm::Value * genChainWalkCall(cg_tid_t tid, branch_id_t branchId);

    /// The scan runs morsel-driven if only selections lie between it and the result, or selections and maps between it
    /// and an aggregation (whose workers aggregate into groups of their own)
    bool isParallelPipeline();
    void produceParallel();
    ParallelState * _parallel = nullptr;

    /// Scans the candidates of a branch-local index instead of the whole table
    void produceIndexProbe(const BranchIndex & index, const std::string & key);
    cg_bool_t nullPointerCheck(cg_voidptr_t &pointer);
    llvm::Value *tupleToElemPtr(cg_voidptr_t &ptr, column_t &column);

    cg_bool_t isVisible(cg_tid_t tid, cg_branch_id_t branchId);
    llvm::Value *getMasterElemPtr(cg_tid_t &tid, column_t &column);
    llvm::Value *getBranchElemPtr(cg_tid_t &tid, column_t &column, cg_voidptr_t &resultPtr, cg_bool_t &ptrIsNotNull);

    Table & table;
    branch_id_t branchId;

    std::vector<column_t> columns;
    Sql::value_op_t tidSqlValue;
};

} // end namespace Physical
} // end namespace Algebra
