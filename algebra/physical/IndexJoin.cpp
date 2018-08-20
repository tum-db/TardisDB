//
// Created by josef on 30.12.16.
//

#include "IndexJoin.hpp"

#include <algorithm>
#include <memory>

#include <llvm/IR/TypeBuilder.h>

#include "codegen/CodeGen.hpp"
#include "foundations/StaticHashtable.hpp"
#include "foundations/MemoryPool.hpp"
#include "foundations/utils.hpp"
#include "sql/SqlType.hpp"
#include "sql/SqlValues.hpp"
#include "sql/SqlTuple.hpp"

using namespace Sql;

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
        UnaryOperator(std::move(probe), required),
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
    child->produce();
}

void IndexJoin::probeCandidate(cg_voidptr_t rawNodePtr)
{
    assert(listNodeTy != nullptr);

//    Functions::genPrintfCall("===\nprobeCandidate() iter node: %p\n===\n", rawNodePtr);

    auto & codeGen = getThreadLocalCodeGen();
    auto & funcGen = codeGen.getCurrentFunctionGen();

    llvm::Type * nodePtrTy = llvm::PointerType::getUnqual(listNodeTy);

    // load the tuple
    llvm::Value * nodePtr = codeGen->CreatePointerCast(rawNodePtr, nodePtrTy);
    llvm::Value * tuplePtr = codeGen->CreateStructGEP(listNodeTy, nodePtr, 2);
    auto tuple = SqlTuple::load(tuplePtr, storedTypes);

    // check if all related values do also match:
#if 0
    // naive first version:

    cg_bool_t all(true);
    for (auto & joinPair : joinPairs) {
        auto it = tupleMapping.find(joinPair.first);
        iu_p_t rightCIU = joinPair.second;
        assert(rightProduced->count(rightCIU) == 1);

        size_t i = it->second;
        SqlValue & left = *(tuple->values[i]);
        SqlValue & right = *(rightProduced->at(rightCIU));

        all = all && left.equals(right);
    }

    IfGen check(funcGen, all);
    {
/*
        Functions::genPrintfCall("RESULT: ");
        genPrintSqlValue(tuple.values[0]);
        Functions::genPrintfCall(", ");
        genPrintSqlValue(*rightProduced[0]);
        Functions::genPrintfCall("\n");
*/
#if 0
        // merge both sides
        iu_value_mapping_t values(*rightProduced);
#endif
        // merge both sides
        iu_value_mapping_t values;
        for (iu_p_t iu : probeSet) {
            values[iu] = rightProduced->at(iu);
        }
        for (auto & pair : tupleMapping) {
            values[pair.first] = tuple->values[pair.second].get();
        }

        parent->consume(values, *this);
    }
    check.EndIf();
#else
    // this version builds a nested "if" construct in order to avoid unnecessary comparisons

    // "IfGen"s are not copyable (otherwise the BasicBlock structure would get mixed up)
    std::vector<std::unique_ptr<IfGen>> chain;

    for (auto pairIt = joinPairs.begin(); pairIt != joinPairs.end(); ++pairIt) {
        auto & joinPair = *pairIt;

        auto tupleIt = tupleMapping.find(joinPair.first);
        iu_p_t rightIU = joinPair.second;
        assert(rightProduced->count(rightIU) == 1);

        size_t i = tupleIt->second;
        Value & left = *(tuple->values[i]);
        Value & right = *(rightProduced->at(rightIU));

        // extend the nested "if"-chain by one statement
        cg_bool_t equals = left.equals(right);
        chain.emplace_back( std::make_unique<IfGen>(funcGen, equals) );

        // at this point all join-attributes are guaranteed to be equal
        if (std::next(pairIt) == joinPairs.end()) {
#if 0
            // merge both sides
            iu_value_mapping_t values(*rightProduced);
#endif
            // merge both sides
            iu_value_mapping_t values;
            for (iu_p_t iu : probeSet) {
                values[iu] = rightProduced->at(iu);
            }
            for (auto & pair : tupleMapping) {
                values[pair.first] = tuple->values[pair.second].get();
            }

            parent->consume(values, *this);
        }
    }

    // close the chain in reverse order
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        it->get()->EndIf();
    }
#endif
}

tid_t performLookup(const iu_value_mapping_t & values)
{

}

tid_t performRangeLookup(const iu_value_mapping_t & values)
{
    
}

void IndexJoin::consume(const iu_value_mapping_t & values, const Operator & src)
{

    cg_voidptr_t key;

    // lookup
    cg_tid_t tid;
    if (method == LookupMethod::Exact) {
        tid = performLookup(values);
    } else {
        tid = performLookup(values);
    }

    // load
    //...
    
}

} // end namespace Physical
} // end namespace Algebra
