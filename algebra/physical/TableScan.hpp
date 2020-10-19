
#pragma once

#include "algebra/physical/Operator.hpp"
#include "foundations/Database.hpp"
#include "sql/SqlValues.hpp"

namespace Algebra {
namespace Physical {

/// The table scan operator
class TableScan : public NullaryOperator {
public:
    TableScan(const logical_operator_t & logicalOperator, Table & table, branch_id_t branchId);

    virtual ~TableScan();

    void produce() override;

#if USE_DATA_VERSIONING
    void produce(cg_tid_t tid, branch_id_t branchId);
#else
    void produce(cg_tid_t tid);
#endif

private:
    using column_t = std::tuple<ci_p_t, llvm::Type *, llvm::Value *, size_t, Sql::value_op_t>;

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
