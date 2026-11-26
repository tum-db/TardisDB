#include "algebra/physical/IndexJoin.hpp"

#include "codegen/LLVMCompat.hpp"

#include "algebra/physical/Select.hpp"
#include "algebra/physical/TableScan.hpp"
#include "codegen/CodeGen.hpp"
#include "native/sql/SqlValues.hpp"
#include "sql/SqlValues.hpp"

namespace Algebra {
namespace Physical {

/// \returns The number of tuples of the key in the buffer of the state
static size_t index_join_lookup(void * state)
{
    auto * s = static_cast<IndexJoin::State *>(state);
    Native::Sql::value_op_t key = Native::Sql::Value::load(s->buffer.data(), s->type);
    s->tids = s->index->lookup(BranchIndex::key(*key));
    return (s->tids != nullptr) ? s->tids->size() : 0;
}

/// \returns The tuples of the last lookup
static void * index_join_tids(void * state)
{
    auto * s = static_cast<IndexJoin::State *>(state);
    return (s->tids != nullptr) ? const_cast<tid_t *>(s->tids->data()) : nullptr;
}

IndexJoin::IndexJoin(const logical_operator_t & logicalOperator,
                     std::unique_ptr<Operator> probe, std::unique_ptr<Operator> indexed,
                     const BranchIndex & index, join_pair_vec_t pairs, size_t keyPair, QueryContext & queryContext) :
        BinaryOperator(logicalOperator, std::move(probe), std::move(indexed), queryContext),
        _index(index),
        _pairs(std::move(pairs)),
        _keyPair(keyPair),
        _scan(indexedScan(*_rightChild))
{
    assert(_scan != nullptr && _keyPair < _pairs.size());
}

IndexJoin::~IndexJoin()
{ }

TableScan * IndexJoin::indexedScan(Operator & indexed)
{
    Operator * op = &indexed;
    while (dynamic_cast<Select *>(op) != nullptr) op = op->getChildren().front();
    return dynamic_cast<TableScan *>(op);
}

void IndexJoin::produce()
{
    // an empty indexed table joins nothing
    if (!_scan->prepare()) return;

    auto owned = std::make_unique<State>();
    _state = owned.get();
    _context.executionContext.acquireResource(std::move(owned));
    _state->index = &_index;
    _state->type = Sql::toNotNullableTy(_pairs[_keyPair].second->getType());
    llvm::Type * type = Sql::toLLVMTy(_state->type);
    _state->buffer.resize(_codeGen.getCurrentModuleGen().getModule().getDataLayout().getTypeAllocSize(type));

    _leftChild->produce();
}

void IndexJoin::consumeLeft(const iu_value_mapping_t & values)
{
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();

    _probeValues = &values;
    _probeKeys.clear();
    for (auto & pair : _pairs) _probeKeys.push_back(pair.first->evaluate(values));

    // a NULL key finds nothing
    const Sql::Value * key = _probeKeys[_keyPair].get();
    cg_bool_t present(true);
    if (Sql::isNullable(*key)) {
        present = !static_cast<const Sql::NullableValue &>(*key).isNull();
        key = &Sql::getAssociatedValue(*key);
    }
    IfGen check(present);
    {
        llvm::Value * state = cg_ptr8_t::fromRawPointer(_state).getValue();
        key->store(cg_ptr8_t::fromRawPointer(_state->buffer.data()).getValue());
        llvm::Value * count = codeGen.CreateCall(&index_join_lookup, llvm::TypeBuilder<size_t (void *), false>::get(context), {state});
        llvm::Value * tids = codeGen->CreatePointerCast(
                codeGen.CreateCall(&index_join_tids, llvm::TypeBuilder<void * (void *), false>::get(context), {state}),
                llvm::PointerType::getUnqual(llvm::Type::getInt64Ty(context)));
        _scan->produceCandidates(count, tids);
    }
    check.EndIf();

    _probeValues = nullptr;
}

void IndexJoin::consumeRight(const iu_value_mapping_t & values)
{
    assert(_probeValues != nullptr);

    // every pair, also that of the index, whose entries are candidates
    cg_bool_t match(true);
    for (size_t i = 0; i < _pairs.size(); ++i) {
        auto indexedValue = _pairs[i].second->evaluate(values);
        match = match && _probeKeys[i]->equals(*indexedValue);
    }

    IfGen check(match);
    {
        iu_value_mapping_t joined = *_probeValues;
        for (auto & [iu, value] : values) joined[iu] = value;
        emit(joined);
    }
    check.EndIf();
}

} // end namespace Physical
} // end namespace Algebra
