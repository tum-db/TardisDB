// GROUP BY in a hash table of the groups, with the aggregates of their tuples (see Logical::Aggregation)

#pragma once

#include "algebra/physical/Operator.hpp"
#include "algebra/logical/operators.hpp"
#include "queryExecutor/ExecutionContext.hpp"

namespace Native { namespace Sql { namespace Arrays { struct NumberArray; class Arena; } } }

namespace Algebra {
namespace Physical {

/// A group holds its keys and, per aggregate, the count of its values that are not NULL and its state: the sum
/// (a 128-bit integer of integers, bigints and numerics, a double of doubles, a runtime array of arrays) or the
/// least or greatest value. Without keys there is one group, emitted also without tuples.
class Aggregation : public UnaryOperator {
public:
    /// The runtime state: the sums of arrays and the arena of their results
    struct State : public ExecutionResource {
        std::vector<std::unique_ptr<Native::Sql::Arrays::NumberArray>> arrays;
        Native::Sql::Arrays::Arena * arena = nullptr;
        std::vector<Sql::SqlType> elementTypes; // of the arrays, per aggregate
        ~State() override;
    };

    Aggregation(const logical_operator_t & logicalOperator, std::unique_ptr<Operator> input, QueryContext & queryContext);

    ~Aggregation() override;

    void produce() override;

    void consume(const iu_value_mapping_t & values, const Operator & src) override;

private:
    using Function = Logical::Aggregation::Function;

    const Logical::Aggregation & logical() const;

    /// The value of a key in the type of its column (a NULL indicator if it is nullable)
    Sql::value_op_t keyValue(const Sql::Value & value, Sql::SqlType type) const;

    cg_bool_t keysEqual(cg_voidptr_t group, const std::vector<Sql::value_op_t> & keys);

    void update(cg_voidptr_t group, const iu_value_mapping_t & values);

    void emitGroup(cg_voidptr_t group);

    llvm::Value * field(cg_voidptr_t group, unsigned index);

    std::vector<Sql::SqlType> _keyTypes;
    std::vector<Sql::SqlType> _inputTypes; // of the aggregates, without NULL
    llvm::StructType * _groupType = nullptr;
    size_t _groupSize = 0;
    State * _state = nullptr;
    cg_voidptr_t _hashTable;
    llvm::Value * _singleGroup = nullptr;
};

} // end namespace Physical
} // end namespace Algebra
