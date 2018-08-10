
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

private:
    Table & table;
};

} // end namespace Physical
} // end namespace Algebra
