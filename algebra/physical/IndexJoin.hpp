
#pragma once

#include "algebra/physical/Operator.hpp"
#include "algebra/physical/expressions.hpp"

#include "third_party/ART/Tree.h"

namespace Algebra {
namespace Physical {

class IndexJoin : public BinaryOperator {
public:
    using join_pair_vec_t = std::vector<std::pair<iu_p_t, Expressions::exp_op_t>>;

    enum LookupMethod {
        Exact,
        Range
    };

    IndexJoin(
        std::unique_ptr<Operator> probe, // left
        std::unique_ptr<Operator> indexed, // right
        const iu_set_t & required,
        ART_unsynchronized::Tree & index,
        const join_pair_vec_t & joinPairs, // expected to be sorted
        LookupMethod method
    );

    virtual ~IndexJoin();

    virtual void produce() override;

private:
    void constructIUSets();

    void probeCandidate(cg_voidptr_t nodePtr);

    void consumeLeft(const iu_value_mapping_t & values) override;
    void consumeRight(const iu_value_mapping_t & values) override;

    // stored tuple description
//    std::vector<Sql::SqlType> storedTypes; // used to construct the SqlTuple
//    std::unordered_map<iu_p_t, size_t> tupleMapping; // iu_p_t -> index into storedTypes

    std::vector<Sql::SqlType> lookupType; // ordered

    iu_set_t probeSet;
    iu_set_t indexedSet;

    ART_unsynchronized::Tree & index;

    join_pair_vec_t joinPairs;

    LookupMethod method;
};

} // end namespace Physical
} // end namespace Algebra
