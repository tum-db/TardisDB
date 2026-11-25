
#pragma once

#include "algebra/physical/Operator.hpp"
#include "algebra/physical/expressions.hpp"
#include "sql/SqlTuple.hpp"

/// The tuples of a side of a join in a list in a memory pool (HashJoin.cpp): a node holds the next node, a
/// hash and the tuple
llvm::Value * genCreateListHeader(cg_voidptr_t memoryPool);
llvm::Type * getListNodeTy(llvm::Type * dataTy);
void genAppendEntryToList(cg_voidptr_t memoryPool, llvm::Value * headerPtr, llvm::Type * nodeTy, cg_hash_t hash, Sql::SqlTuple & tuple);
std::pair<cg_voidptr_t, cg_size_t> genListGetHeaderData(llvm::Value * headerPtr);

namespace Algebra {
namespace Physical {

/// The hash join operator
class HashJoin : public BinaryOperator {
public:
    using join_pair_vec_t = std::vector<std::pair<Expressions::exp_op_t, Expressions::exp_op_t>>;

    /// \param pairs: vector of (left expr, right expr) pairs
    HashJoin(const logical_operator_t & logicalOperator,
            std::unique_ptr<Operator> left, std::unique_ptr<Operator> right,
            join_pair_vec_t pairs, QueryContext &queryContext);

    virtual ~HashJoin();

    virtual void produce() override ;

private:
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

    const iu_value_mapping_t * rightIncoming;
};

} // end namespace Physical
} // end namespace Algebra
