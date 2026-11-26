// WITH RECURSIVE ... UNION ALL as a loop of the generated code (see Logical::Fixpoint): buffers of rows for the
// working table, the rows of the current step and the result

#pragma once

#include "algebra/physical/Operator.hpp"
#include "algebra/logical/operators.hpp"
#include "queryExecutor/ExecutionContext.hpp"

namespace Algebra {
namespace Physical {

/// Rows of the columns of a common table expression, each an LLVM struct of their values
struct FixpointBuffer {
    std::vector<uint8_t> data;
    size_t rows = 0;
};

/// The buffers of a recursive query, and its columns
struct FixpointState : public ExecutionResource {
    std::vector<std::string> names;
    std::vector<Sql::SqlType> types; // nullable
    size_t rowSize = 0;
    FixpointBuffer working, next, result;

    /// \returns The struct of a row
    llvm::StructType * rowType() const;
};

class Fixpoint : public BinaryOperator {
public:
    Fixpoint(const logical_operator_t & logicalOperator, std::unique_ptr<Operator> initial, std::unique_ptr<Operator> step,
             FixpointState & state, QueryContext & queryContext);

    ~Fixpoint() override;

    void produce() override;

private:
    void consumeLeft(const iu_value_mapping_t & values) override;
    void consumeRight(const iu_value_mapping_t & values) override;

    /// Appends the values of the columns as a row of the buffer
    void append(FixpointBuffer & buffer, const std::vector<iu_p_t> & columns, const iu_value_mapping_t & values);

    const Logical::Fixpoint & logical() const;

    FixpointState & _state;
};

/// The scan of the working table of a recursive query: the rows the last step added
class WorkingTableScan : public NullaryOperator {
public:
    WorkingTableScan(const logical_operator_t & logicalOperator, FixpointState & state, QueryContext & queryContext);

    ~WorkingTableScan() override;

    void produce() override;

private:
    FixpointState & _state;
};

} // end namespace Physical
} // end namespace Algebra
