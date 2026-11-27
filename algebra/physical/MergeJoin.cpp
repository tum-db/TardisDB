#include "algebra/physical/MergeJoin.hpp"

#include <algorithm>
#include <cstddef>

#include "codegen/LLVMCompat.hpp"
#include "foundations/MemoryPool.hpp"
#include "runtime/SqlValues.hpp"
#include "sql/SqlTuple.hpp"
#include "sql/SqlValues.hpp"

using namespace Sql;

namespace Algebra {
namespace Physical {

/// The part in C++: sorts the stored tuples of both sides by their keys, then finds the groups of equal keys
struct MergeJoin::State : public ExecutionResource {
    struct Group {
        size_t left[2];  // [begin, end) of the sorted tuples of the left side
        size_t right[2]; // ... of the right side
    };
    struct Side {
        size_t tupleOffset = 0;         // of the tuple in a node of the list
        std::vector<size_t> keyOffsets; // of the keys in the tuple
        std::vector<SqlType> keyTypes;
        std::vector<void *> nodes;      // sorted by their keys, without those with a NULL key
        std::vector<std::vector<Native::Sql::value_op_t>> keys; // of the nodes, while they are merged
    };
    Side sides[2];
    std::vector<Group> groups;
};

using State = MergeJoin::State;

static int compareKeys(const std::vector<Native::Sql::value_op_t> & a, const std::vector<Native::Sql::value_op_t> & b)
{
    for (size_t k = 0; k < a.size(); ++k) {
        const int c = Native::Sql::compareValues(Native::Sql::getAssociatedValue(*a[k]), Native::Sql::getAssociatedValue(*b[k]));
        if (c != 0) return c;
    }
    return 0;
}

/// Sorts the \p length nodes of a list from \p first by their keys
static void sortSide(State::Side & side, void * first, size_t length)
{
    std::vector<std::pair<std::vector<Native::Sql::value_op_t>, void *>> rows;
    rows.reserve(length);
    void * node = first;
    for (size_t n = 0; n < length; ++n, node = *static_cast<void **>(node)) { // the first field: the next node
        const uint8_t * tuple = static_cast<const uint8_t *>(node) + side.tupleOffset;
        std::vector<Native::Sql::value_op_t> key;
        bool null = false;
        for (size_t k = 0; k < side.keyTypes.size() && !null; ++k) {
            key.push_back(Native::Sql::Value::load(tuple + side.keyOffsets[k], side.keyTypes[k]));
            null = Native::Sql::isNullable(*key.back()) && static_cast<const Native::Sql::NullableValue &>(*key.back()).isNull();
        }
        if (!null) rows.emplace_back(std::move(key), node); // NULL matches no key
    }
    std::stable_sort(rows.begin(), rows.end(), [](const auto & x, const auto & y) { return compareKeys(x.first, y.first) < 0; });
    side.nodes.clear();
    side.keys.clear();
    for (auto & row : rows) {
        side.keys.push_back(std::move(row.first));
        side.nodes.push_back(row.second);
    }
}

/// \returns The number of groups of equal keys of both sides
static size_t merge_join_run(State * state, void * leftFirst, size_t leftLength, void * rightFirst, size_t rightLength)
{
    auto & left = state->sides[0], & right = state->sides[1];
    sortSide(left, leftFirst, leftLength);
    sortSide(right, rightFirst, rightLength);
    state->groups.clear();
    size_t i = 0, j = 0;
    while (i < left.nodes.size() && j < right.nodes.size()) {
        const int c = compareKeys(left.keys[i], right.keys[j]);
        if (c < 0) { ++i; continue; }
        if (c > 0) { ++j; continue; }
        size_t leftEnd = i + 1, rightEnd = j + 1;
        while (leftEnd < left.nodes.size() && compareKeys(left.keys[leftEnd], left.keys[i]) == 0) ++leftEnd;
        while (rightEnd < right.nodes.size() && compareKeys(right.keys[rightEnd], right.keys[j]) == 0) ++rightEnd;
        state->groups.push_back({{i, leftEnd}, {j, rightEnd}});
        i = leftEnd;
        j = rightEnd;
    }
    for (auto & side : state->sides) std::vector<std::vector<Native::Sql::value_op_t>>().swap(side.keys);
    return state->groups.size();
}

static void * merge_join_groups(State * state)
{
    return state->groups.data();
}

static void * merge_join_nodes(State * state, size_t side)
{
    return state->sides[side].nodes.data();
}

static llvm::Function * declareFunction(const std::string & name, llvm::FunctionType * type, void * function)
{
    auto & moduleGen = getThreadLocalCodeGen().getCurrentModuleGen();
    llvm::Function * func = LLVMCompat::getOrInsertFunction(moduleGen.getModule(), name, type);
    moduleGen.addFunctionMapping(func, function);
    return func;
}

//-----------------------------------------------------------------------------
// MergeJoin

MergeJoin::MergeJoin(const logical_operator_t & logicalOperator, std::unique_ptr<Operator> left, std::unique_ptr<Operator> right,
                     join_pair_vec_t pairs, QueryContext & queryContext) :
        BinaryOperator(std::move(logicalOperator), std::move(left), std::move(right), queryContext),
        joinPairs(std::move(pairs))
{
    assert(canJoin(joinPairs));
}

MergeJoin::~MergeJoin()
{ }

bool MergeJoin::canJoin(const join_pair_vec_t & pairs)
{
    if (pairs.empty()) return false;
    for (auto & [left, right] : pairs) {
        const SqlType a = left->getType(), b = right->getType();
        if (a.typeID != b.typeID || !Native::Sql::hasOrder(a)) return false;
        // the raw values of numerics compare at one scale
        if (a.typeID == SqlType::TypeID::NumericID &&
            (a.numericLayout.length != b.numericLayout.length || a.numericLayout.precision != b.numericLayout.precision)) return false;
    }
    return true;
}

void MergeJoin::store(Side & side, size_t index, const iu_set_t & passed, const iu_value_mapping_t & values)
{
    std::vector<value_op_t> tupleValues;
    side.storedTypes.clear();
    side.mapping.clear();
    for (auto & pair : joinPairs) {
        value_op_t key = (index == 0 ? pair.first : pair.second)->evaluate(values);
        side.storedTypes.push_back(key->type);
        tupleValues.push_back(std::move(key));
    }
    for (iu_p_t iu : passed) {
        value_op_t value = values.at(iu)->clone();
        side.mapping[iu] = side.storedTypes.size();
        side.storedTypes.push_back(value->type);
        tupleValues.push_back(std::move(value));
    }
    SqlTuple tuple(std::move(tupleValues));
    side.nodeTy = getListNodeTy(tuple.getType());
    genAppendEntryToList(memoryPool, side.listHeader, side.nodeTy, cg_hash_t(static_cast<uint64_t>(0)), tuple);
}

void MergeJoin::consumeLeft(const iu_value_mapping_t & values)
{
    store(sides[0], 0, dynamic_cast<const Logical::BinaryOperator &>(_logicalOperator).getLeftRequired(), values);
}

void MergeJoin::consumeRight(const iu_value_mapping_t & values)
{
    store(sides[1], 1, dynamic_cast<const Logical::BinaryOperator &>(_logicalOperator).getRightRequired(), values);
}

void MergeJoin::load(const Side & side, llvm::Value * nodePtr, iu_value_mapping_t & values, std::vector<std::unique_ptr<SqlTuple>> & tuples)
{
    llvm::Value * typedNode = _codeGen->CreatePointerCast(nodePtr, llvm::PointerType::getUnqual(side.nodeTy));
    llvm::Value * tuplePtr = _codeGen->CreateStructGEP(side.nodeTy, typedNode, 2);
    auto tuple = SqlTuple::load(tuplePtr, side.storedTypes);
    for (auto & [iu, index] : side.mapping) values[iu] = tuple->values[index].get();
    tuples.push_back(std::move(tuple));
}

void MergeJoin::produce()
{
    auto & funcGen = _codeGen.getCurrentFunctionGen();
    auto & context = _codeGen.getLLVMContext();
    llvm::Type * i8Ty = llvm::Type::getInt8Ty(context);
    llvm::Type * i8PtrTy = LLVMCompat::getInt8PtrTy(context);
    llvm::Type * i64Ty = llvm::Type::getInt64Ty(context);

    memoryPool = genMemoryPoolCreateCall();
    for (auto & side : sides) side.listHeader = genCreateListHeader(memoryPool);
    _leftChild->produce();
    _rightChild->produce();

    // a side that passes on no tuple (e.g. a selection that is always false) joins nothing
    if (sides[0].nodeTy != nullptr && sides[1].nodeTy != nullptr) {
        auto owned = std::make_unique<State>();
        State & state = *owned;
        _context.executionContext.acquireResource(std::move(owned));
        auto & dataLayout = _codeGen.getCurrentModuleGen().getDataLayout();
        for (size_t s = 0; s < 2; ++s) {
            auto * nodeTy = llvm::cast<llvm::StructType>(sides[s].nodeTy);
            state.sides[s].tupleOffset = dataLayout.getStructLayout(nodeTy)->getElementOffset(2);
            auto * tupleLayout = dataLayout.getStructLayout(llvm::cast<llvm::StructType>(nodeTy->getElementType(2)));
            for (size_t k = 0; k < joinPairs.size(); ++k) {
                state.sides[s].keyOffsets.push_back(tupleLayout->getElementOffset(k));
                state.sides[s].keyTypes.push_back(sides[s].storedTypes[k]);
            }
        }

        llvm::Function * run = declareFunction("merge_join_run",
                llvm::TypeBuilder<size_t (void *, void *, size_t, void *, size_t), false>::get(context), (void *) &merge_join_run);
        llvm::Function * groupsFunc = declareFunction("merge_join_groups", llvm::TypeBuilder<void * (void *), false>::get(context), (void *) &merge_join_groups);
        llvm::Function * nodesFunc = declareFunction("merge_join_nodes", llvm::TypeBuilder<void * (void *, size_t), false>::get(context), (void *) &merge_join_nodes);
        llvm::Value * statePtr = cg_ptr8_t::fromRawPointer(&state).getValue();
        auto left = genListGetHeaderData(sides[0].listHeader), right = genListGetHeaderData(sides[1].listHeader);
        llvm::Value * count = _codeGen->CreateCall(run, {statePtr, left.first.getValue(), left.second.getValue(),
                                                         right.first.getValue(), right.second.getValue()});
        llvm::Value * groups = _codeGen->CreateCall(groupsFunc, {statePtr});
        llvm::Type * nodeArrayTy = llvm::PointerType::getUnqual(i8PtrTy);
        llvm::Value * nodes[2];
        for (size_t s = 0; s < 2; ++s) {
            nodes[s] = _codeGen->CreatePointerCast(_codeGen->CreateCall(nodesFunc, {statePtr, cg_size_t(s).getValue()}), nodeArrayTy);
        }
        auto nodeAt = [&](size_t s, cg_size_t i) {
            return _codeGen->CreateLoad(i8PtrTy, _codeGen->CreateGEP(i8PtrTy, nodes[s], i.getValue()));
        };

        LoopGen groupLoop(funcGen, _codeGen->CreateICmpULT(cg_size_t(0ul).getValue(), count), {{"group", cg_size_t(0ul)}});
        cg_size_t group(groupLoop.getLoopVar(0));
        {
            LoopBodyGen groupBody(groupLoop);
            llvm::Value * groupPtr = _codeGen->CreateGEP(i8Ty, groups, _codeGen->CreateMul(group.getValue(), cg_size_t(sizeof(State::Group)).getValue()));
            auto field = [&](size_t offset) {
                llvm::Value * ptr = _codeGen->CreateGEP(i8Ty, groupPtr, cg_size_t(offset).getValue());
                return cg_size_t(_codeGen->CreateLoad(i64Ty, _codeGen->CreatePointerCast(ptr, llvm::PointerType::getUnqual(i64Ty))));
            };
            cg_size_t leftBegin = field(offsetof(State::Group, left)), leftEnd = field(offsetof(State::Group, left) + sizeof(size_t));
            cg_size_t rightBegin = field(offsetof(State::Group, right)), rightEnd = field(offsetof(State::Group, right) + sizeof(size_t));

            // each tuple of the left side of the group with each of the right side
            LoopGen leftLoop(funcGen, _codeGen->CreateICmpULT(leftBegin.getValue(), leftEnd.getValue()), {{"i", leftBegin}});
            cg_size_t i(leftLoop.getLoopVar(0));
            {
                LoopBodyGen leftBody(leftLoop);
                llvm::Value * leftNode = nodeAt(0, i);
                LoopGen rightLoop(funcGen, _codeGen->CreateICmpULT(rightBegin.getValue(), rightEnd.getValue()), {{"j", rightBegin}});
                cg_size_t j(rightLoop.getLoopVar(0));
                {
                    LoopBodyGen rightBody(rightLoop);
                    iu_value_mapping_t values;
                    std::vector<std::unique_ptr<SqlTuple>> tuples;
                    load(sides[0], leftNode, values, tuples);
                    load(sides[1], nodeAt(1, j), values, tuples);
                    emit(values);
                }
                cg_size_t nextJ = j + 1ul;
                rightLoop.loopDone(_codeGen->CreateICmpULT(nextJ.getValue(), rightEnd.getValue()), {nextJ});
            }
            cg_size_t nextI = i + 1ul;
            leftLoop.loopDone(_codeGen->CreateICmpULT(nextI.getValue(), leftEnd.getValue()), {nextI});
        }
        cg_size_t nextGroup = group + 1ul;
        groupLoop.loopDone(_codeGen->CreateICmpULT(nextGroup.getValue(), count), {nextGroup});
    }

    genMemoryPoolFreeCall(memoryPool);
}

} // end namespace Physical
} // end namespace Algebra
