//
// Created by josef on 30.12.16.
//

#include "IndexJoin.hpp"

#include <algorithm>
#include <memory>

#include <llvm/IR/TypeBuilder.h>

#include "algebra/physical/TableScan.hpp"
#include "codegen/CodeGen.hpp"
#include "foundations/StaticHashtable.hpp"
#include "foundations/MemoryPool.hpp"
#include "foundations/utils.hpp"
#include "sql/SqlType.hpp"
#include "sql/SqlValues.hpp"
#include "sql/SqlTuple.hpp"

using namespace Sql;

#define ENABLE_IJ
#if 0
//-----------------------------------------------------------------------------
// operator implementation

namespace Algebra {
namespace Physical {

IndexJoin::IndexJoin(
        std::unique_ptr<Operator> probe, // left
        std::unique_ptr<Operator> indexed, // right
        const iu_set_t & required,
        ART_unsynchronized::Tree & index,
        const join_pair_vec_t & joinPairs, // expected to be sorted
        LookupMethod method) :
        BinaryOperator(std::move(probe), std::move(indexed), required),
        index(index),
        joinPairs(std::move(joinPairs)),
        method(method)
{
    constructIUSets();
}

IndexJoin::~IndexJoin()
{ }

void IndexJoin::constructIUSets()
{
    // build a set of required ius that contains only ius from the left side
    const iu_set_t & leftChildRequired = leftChild->getRequired();
    std::set_intersection(
            _required.begin(), _required.end(),
            leftChildRequired.begin(), leftChildRequired.end(),
            std::inserter(buildSet, buildSet.end())
    );

    // build a set of required ius that contains only ius from the right side
    const iu_set_t & rightChildRequired = rightChild->getRequired();
    std::set_intersection(
            _required.begin(), _required.end(),
            rightChildRequired.begin(), rightChildRequired.end(),
            std::inserter(probeSet, probeSet.end())
    );

    // TODO assert required == buildSet union probeSet
}

void IndexJoin::produce()
{
    leftChild->produce();
}

void IndexJoin::probeCandidate(cg_voidptr_t rawNodePtr)
{

}

cg_tid_t performARTLookup(const iu_value_mapping_t & values)
{
    // TODO
    auto& executionContext = getContext().getExecutionContext();

    auto indexJoinResources = std::make_unique<IndexJoinResources>();
    executionContext.acquireResource(std::move(indexJoinResources));

    cg_voidptr_t key = cg_voidptr_t::fromRawPointer(&indexJoinResources->key);

}

cg_tid_t performLookup(const iu_value_mapping_t & values)
{
    // TODO

}

cg_tid_t performRangeLookup(const iu_value_mapping_t & values)
{
    // TODO
    throw NotImplementedException();
}

using toKey = void (*)(SqlTuple & tuple, Key & key);

struct IndexJoinResources : public ExecutionResource {
    ART_unsynchronized::Tree::LoadKeyFunction loadKeyFunc;
    Key key;

    std::vector<SqlType> keyType;
};

// TODO
// generic key conversion function
// Tuples as first-class citizens?
// IndexJoin interface
// unique keys only?
// compound keys?
// C++ interop
// Null values?
// Use the TableScan Operator on the indexed table?
// Recursive Function Generators (nesting)

void IndexJoin::consumeLeft(const iu_value_mapping_t & values)
{
    // lookup
    cg_tid_t tid;
    if (method == LookupMethod::Exact) {
        tid = performLookup(values);
    } else {
        tid = performRangeLookup(values);
    }

    IfGen check(tid != cg_tid_t(invalid_tid));
    {
        iu_value_mapping_t generatedValues;

        // load
        auto * tableScan = dynamic_cast<TableScan *>(rightChild.get());
        assert(tableScan);
        tableScan->produce(tid);
    }
    check.EndIf();
}

void IndexJoin::consumeRight(const iu_value_mapping_t & values)
{
    iu_value_mapping_t generatedValues; // TODO

    for (iu_p_t iu : probeSet) {
        generatedValues[iu] = values.at(iu);
    }
    for (auto & pair : tupleMapping) {
        generatedValues[pair.first] = tuple->values[pair.second].get();
    }

    parent->consume(generatedValues, *this);
}

} // end namespace Physical
} // end namespace Algebra
#endif
