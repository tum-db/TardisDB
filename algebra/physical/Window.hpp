// Window functions in the plan (see Logical::Window): a pipeline breaker that buffers its input, computes the functions (WindowEvaluation) and passes the tuples on with them

#pragma once

#include <memory>

#include "algebra/physical/Operator.hpp"
#include "algebra/physical/WindowEvaluation.hpp"
#include "algebra/logical/operators.hpp"
#include "queryExecutor/ExecutionContext.hpp"

namespace Algebra {
namespace Physical {

/// A row is an LLVM struct of the attributes it reads or passes on, then of the results; workers insert into hash groups of their own
class Window : public UnaryOperator {
public:
    struct State : public ExecutionResource {
        std::unique_ptr<WindowEvaluation::Rows> rows;
    };

    Window(const logical_operator_t & logicalOperator, std::unique_ptr<Operator> input, QueryContext & queryContext);

    ~Window() override;

    void produce() override;

    void consume(const iu_value_mapping_t & values, const Operator & src) override;

    /// \returns Whether the operators above take the tuples from workers: selections and maps up to an aggregation or a window, selections up to the result
    bool emitsParallel() const;

private:
    const Logical::Window & logical() const;

    /// \returns Whether the scan or window below (through selections and maps) passes tuples from workers
    bool parallelInput() const;

    /// Passes the row at \p index on to the parent
    void emitRow(llvm::Value * index);

    llvm::Value * rowsPointer();

    std::vector<iu_p_t> _stored;   // the fields of a row: the attributes, then the results
    size_t _attributes = 0;        // the first fields of a row, which consume() stores
    llvm::StructType * _rowType = nullptr;
    State * _state = nullptr;
    bool _parallel = false;
};

} // end namespace Physical
} // end namespace Algebra
