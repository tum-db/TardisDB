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
        const logical_operator_t & logicalOperator,
        std::unique_ptr<Operator> probe, // left
        std::unique_ptr<Operator> indexed, // right
        ART_unsynchronized::Tree & index,
        join_pair_vec_t joinPairs, // expected to be sorted
        LookupMethod method) :
        BinaryOperator(logicalOperator, std::move(probe), std::move(indexed)),
        _index(index),
        _joinPairs(std::move(joinPairs)),
        _method(method)
{ }

IndexJoin::~IndexJoin()
{ }

void IndexJoin::produce()
{
    _leftChild->produce();
}

void IndexJoin::probeCandidate(cg_voidptr_t rawNodePtr)
{

}

using toKey = void (*)(SqlTuple & tuple, Key & key);

struct IndexJoinResources : public ExecutionResource {
    virtual ~IndexJoinResources() { }

    ART_unsynchronized::Tree::LoadKeyFunction loadKeyFunc;
    Key key;

    std::vector<SqlType> keyType;
};

cg_tid_t IndexJoin::performARTLookup(const iu_value_mapping_t & values)
{
    // TODO
    auto & executionContext = _context.executionContext;

    auto indexJoinResources = std::make_unique<IndexJoinResources>();
    executionContext.acquireResource(std::move(indexJoinResources));

    cg_voidptr_t key = cg_voidptr_t::fromRawPointer(&indexJoinResources->key);


    return cg_tid_t(0ul);
}

cg_tid_t IndexJoin::performBTreeLookup(const iu_value_mapping_t & values)
{
    // TODO
    return cg_tid_t(0ul);
}

cg_tid_t IndexJoin::performLookup(const iu_value_mapping_t & values)
{
    // TODO

    for (auto & pair : _joinPairs) {
        
    }

    return cg_tid_t(0ul);
}

cg_tid_t IndexJoin::performRangeLookup(const iu_value_mapping_t & values)
{
    // TODO
    throw NotImplementedException();
}

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
    if (_method == LookupMethod::Exact) {
        tid = performLookup(values);
    } else {
        tid = performRangeLookup(values);
    }

    _leftIncoming = & values;

    IfGen check(tid != cg_tid_t(invalid_tid));
    {
        iu_value_mapping_t generatedValues;

        // load
        auto * tableScan = dynamic_cast<TableScan *>(_rightChild.get());
        assert(tableScan);
        tableScan->produce(tid);
    }
    check.EndIf();

    _leftIncoming = nullptr;
}

void IndexJoin::consumeRight(const iu_value_mapping_t & values)
{
    assert(_leftIncoming != nullptr);

    // concatenate both sides
    iu_value_mapping_t generatedValues;
    auto & rightRequired = dynamic_cast<const Logical::BinaryOperator &>(_logicalOperator).getRightRequired();
    for (iu_p_t iu : rightRequired) {
        generatedValues[iu] = values.at(iu);
    }
    auto & leftRequired = dynamic_cast<const Logical::BinaryOperator &>(_logicalOperator).getLeftRequired();
    for (iu_p_t iu : leftRequired) {
        generatedValues[iu] = _leftIncoming->at(iu);
    }

    _parent->consume(generatedValues, *this);
}

} // end namespace Physical
} // end namespace Algebra
#endif
