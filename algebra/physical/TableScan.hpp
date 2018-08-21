
#pragma once

#include "algebra/physical/Operator.hpp"

namespace Algebra {
namespace Physical {

/// The table scan operator
class TableScan : public NullaryOperator {
public:
    TableScan(QueryContext & context, const iu_set_t & required, Table & table);

    virtual ~TableScan();

    void produce() override;

    void produce(cg_tid_t tid);

private:
    Table & table;

    using column_t = std::tuple<ci_p_t, llvm::Type *, llvm::Value *>;
    std::vector<column_t> columns;
};

} // end namespace Physical
} // end namespace Algebra
