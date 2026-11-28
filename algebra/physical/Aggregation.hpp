// GROUP BY in a hash table of the groups, with the aggregates of their tuples (see Logical::Aggregation)

#pragma once

#include <mutex>

#include "algebra/physical/Operator.hpp"
#include "algebra/logical/operators.hpp"
#include "foundations/Hashtable.hpp"
#include "queryExecutor/ExecutionContext.hpp"

namespace Native { namespace Sql { namespace Arrays { struct NumberArray; class Arena; } } }

namespace Algebra {
namespace Physical {

/// A group holds its keys and, per aggregate, the count of its values that are not NULL and its state: the sum
/// (a 128-bit integer of integers, bigints and numerics, a double of doubles, a runtime array of arrays) or the
/// least or greatest value. Without keys there is one group, emitted also without tuples. Below a parallel scan
/// each worker aggregates into groups of its own, which are merged after the scan.
class Aggregation : public UnaryOperator {
public:
    /// The runtime state: the sums of arrays and the arena of their results, and the groups of the workers
    struct State : public ExecutionResource {
        std::vector<std::unique_ptr<Native::Sql::Arrays::NumberArray>> arrays;
        std::mutex arraysLatch;
        Native::Sql::Arrays::Arena * arena = nullptr;
        std::vector<Sql::SqlType> elementTypes;           // of the arrays, per aggregate
        size_t workers = 1;
        std::vector<uint8_t> groups;                      // of the workers, without keys
        std::vector<std::unique_ptr<Hashtable>> tables;   // of the workers, with keys
        ~State() override;
    };

    Aggregation(const logical_operator_t & logicalOperator, std::unique_ptr<Operator> input, QueryContext & queryContext);

    ~Aggregation() override;

    void produce() override;

    void consume(const iu_value_mapping_t & values, const Operator & src) override;

private:
    using Function = Logical::Aggregation::Function;

    const Logical::Aggregation & logical() const;

    /// \returns Whether the scan or window below (through selections and maps) passes tuples from workers
    bool parallelInput() const;

    /// The value of a key in the type of its column (a NULL indicator if it is nullable)
    Sql::value_op_t keyValue(const Sql::Value & value, Sql::SqlType type) const;

    cg_hash_t hash(const std::vector<Sql::value_op_t> & keys);

    cg_bool_t keysEqual(cg_voidptr_t group, const std::vector<Sql::value_op_t> & keys);

    /// \returns The node of the group of the keys in the hash table, or nullptr
    cg_voidptr_t lookup(cg_voidptr_t table, const std::vector<Sql::value_op_t> & keys, cg_hash_t hash);

    /// \returns The group of the keys, inserted into the hash table (without values) if it has none
    cg_voidptr_t group(cg_voidptr_t table, const std::vector<Sql::value_op_t> & keys, cg_hash_t hash);

    void update(cg_voidptr_t group, const iu_value_mapping_t & values);

    /// Adds the aggregates of the group \p src (of a worker) to those of \p dst
    void merge(cg_voidptr_t dst, cg_voidptr_t src);

    void emitGroup(cg_voidptr_t group);

    llvm::Value * field(cg_voidptr_t group, unsigned index);

    llvm::Value * statePointer();

    /// The bytes between the groups of two workers (a multiple of a cache line), and the first of them
    size_t workerStride() const;
    llvm::Value * workerGroups();

    /// Generates a loop over the workers, with the index of each
    void forEachWorker(const std::function<void(llvm::Value *)> & body);

    std::vector<Sql::SqlType> _keyTypes;
    std::vector<Sql::SqlType> _inputTypes; // of the aggregates, without NULL
    llvm::StructType * _groupType = nullptr;
    size_t _groupSize = 0;
    State * _state = nullptr;
    bool _parallel = false;
    cg_voidptr_t _hashTable;
    llvm::Value * _singleGroup = nullptr;
};

} // end namespace Physical
} // end namespace Algebra
