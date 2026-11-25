#pragma once

#include "algebra/physical/HashJoin.hpp"

namespace Algebra {
namespace Physical {

/// The sort-merge join: both sides are stored with the values of their keys, sorted by them (in C++), and
/// merged; each group of equal keys joins each of its tuples of the left side with each of the right side.
/// A key that is NULL matches nothing. The keys of a pair have one type with an order (Native::Sql::hasOrder()).
class MergeJoin : public BinaryOperator {
public:
    using join_pair_vec_t = HashJoin::join_pair_vec_t;

    /// \param pairs: (left expression, right expression) pairs, at least one
    MergeJoin(const logical_operator_t & logicalOperator, std::unique_ptr<Operator> left, std::unique_ptr<Operator> right,
              join_pair_vec_t pairs, QueryContext & queryContext);

    ~MergeJoin() override;

    void produce() override;

    /// \returns Whether a merge join can join on \p pairs: keys of one type each with an order
    static bool canJoin(const join_pair_vec_t & pairs);

    struct State;

private:
    void consumeLeft(const iu_value_mapping_t & values) override;
    void consumeRight(const iu_value_mapping_t & values) override;

    /// A side: its list of tuples, the keys first, then the columns that the join passes on
    struct Side {
        llvm::Value * listHeader = nullptr;
        llvm::Type * nodeTy = nullptr;
        std::vector<Sql::SqlType> storedTypes;
        std::unordered_map<iu_p_t, size_t> mapping; // iu -> index into storedTypes
    };

    void store(Side & side, size_t index, const iu_set_t & passed, const iu_value_mapping_t & values);
    void load(const Side & side, llvm::Value * nodePtr, iu_value_mapping_t & values, std::vector<std::unique_ptr<Sql::SqlTuple>> & tuples);

    join_pair_vec_t joinPairs;
    cg_voidptr_t memoryPool;
    Side sides[2];
};

} // end namespace Physical
} // end namespace Algebra
