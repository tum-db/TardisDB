
#pragma once

#include "algebra/physical/Operator.hpp"
#include "algebra/physical/expressions.hpp"

namespace Algebra {
namespace Physical {

/// The hash join operator
class HashJoin : public BinaryOperator {
public:
    using join_pair_vec_t = std::vector<std::pair<Expressions::exp_op_t, Expressions::exp_op_t>>;

    /// \param pairs: vector of (left expr, right expr) pairs
    HashJoin(std::unique_ptr<Operator> left, std::unique_ptr<Operator> right, const iu_set_t & required,
            join_pair_vec_t pairs);

    virtual ~HashJoin();

    virtual void produce() override ;

private:
    void constructIUSets();

    void probeCandidate(cg_voidptr_t nodePtr);

    void consumeLeft(const iu_value_mapping_t & values) override;
    void consumeRight(const iu_value_mapping_t & values) override;

    join_pair_vec_t joinPairs;

    cg_voidptr_t memoryPool;
    cg_voidptr_t joinTable;
    llvm::Value * listHeaderPtr;
    llvm::Type * listNodeTy = nullptr;

    // stored tuple description
    std::vector<Sql::SqlType> storedTypes; // used to construct the SqlTuple
    std::unordered_map<iu_p_t, size_t> tupleMapping; // iu_p_t -> index into storedTypes

    iu_set_t buildSet;
    iu_set_t probeSet;

    const iu_value_mapping_t * rightProduced;
};

} // end namespace Physical
} // end namespace Algebra
