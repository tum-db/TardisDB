#pragma once

#include "algebra/physical/Operator.hpp"
#include "algebra/physical/expressions.hpp"
#include "foundations/Database.hpp"

namespace Algebra {
namespace Physical {

class TableScan;

/// The index nested loop join: for each tuple of the probe side (left), the key of one join pair looks
/// up the tuples of the indexed side (right: a scan of a table, also below selections) in a BranchIndex
/// (an adaptive radix tree), and the scan produces those that are visible in its branch. Since the index
/// holds candidates, every pair of the join is compared on them, as in the hash join.
class IndexJoin : public BinaryOperator {
public:
    /// the expressions of the probe side and of the indexed side
    using join_pair_vec_t = std::vector<std::pair<Expressions::exp_op_t, Expressions::exp_op_t>>;

    IndexJoin(const logical_operator_t & logicalOperator,
              std::unique_ptr<Operator> probe, std::unique_ptr<Operator> indexed,
              const BranchIndex & index, join_pair_vec_t pairs, size_t keyPair, QueryContext & queryContext);

    ~IndexJoin() override;

    void produce() override;

    const BranchIndex & getIndex() const { return _index; }

    /// The scan of the indexed side, below its selections, or nullptr
    static TableScan * indexedScan(Operator & indexed);

    /// The state of the lookups at run time
    struct State : public ExecutionResource {
        const BranchIndex * index;
        Sql::SqlType type;            // of the key, not nullable
        std::vector<uint8_t> buffer;  // the key, stored by the generated code
        const std::vector<tid_t> * tids = nullptr;
    };

private:
    void consumeLeft(const iu_value_mapping_t & values) override;
    void consumeRight(const iu_value_mapping_t & values) override;

    const BranchIndex & _index;
    join_pair_vec_t _pairs;
    size_t _keyPair; // the pair whose indexed expression is the indexed column
    TableScan * _scan;
    State * _state = nullptr;

    const iu_value_mapping_t * _probeValues = nullptr;
    std::vector<Sql::value_op_t> _probeKeys; // of the current probe tuple
};

} // end namespace Physical
} // end namespace Algebra
