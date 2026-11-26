#include "algebra/physical/Aggregation.hpp"

#include "codegen/LLVMCompat.hpp"

#include <stdexcept>

#include "codegen/PhiNode.hpp"
#include "foundations/Hashtable.hpp"
#include "native/sql/Array.hpp"
#include "sql/SqlTuple.hpp"
#include "sql/SqlValues.hpp"

namespace Algebra {
namespace Physical {

using namespace Native::Sql::Arrays;

Aggregation::State::~State() = default;

/// Adds an array to the sum of an aggregate (\p slot: its NumberArray, nullptr before the first)
static void aggregation_array_add(void * state, uint32_t aggregate, void ** slot, const void * handle)
{
    auto * s = static_cast<Aggregation::State *>(state);
    NumberArray numbers = toNumbers(loadHandle(handle), s->elementTypes[aggregate]);
    auto * sum = static_cast<NumberArray *>(*slot);
    if (sum == nullptr) {
        s->arrays.push_back(std::make_unique<NumberArray>(std::move(numbers)));
        *slot = s->arrays.back().get();
        return;
    }
    if (numbers.widths != sum->widths) throw std::runtime_error("sum of arrays of different shapes");
    for (size_t i = 0; i < numbers.values.size(); ++i) {
        sum->values[i] += numbers.values[i];
        if (numbers.nulls[i]) sum->nulls[i] = true;
    }
}

/// Writes the handle of the sum (or the average) of an aggregate of arrays into \p result
static void aggregation_array_result(void * state, void * sum, uint64_t count, bool average, void * result)
{
    auto * s = static_cast<Aggregation::State *>(state);
    NumberArray numbers = sum != nullptr ? *static_cast<NumberArray *>(sum) : NumberArray();
    if (average && count > 0) for (double & x : numbers.values) x /= static_cast<double>(count);
    storeHandle(result, fromNumbers(numbers, s->arena));
}

/// Frees the sums of arrays of the last execution
static void aggregation_reset(void * state)
{
    static_cast<Aggregation::State *>(state)->arrays.clear();
}

/// \returns The next node of the bucket with the hash, or nullptr
static void * aggregation_next(void * node, uint64_t hash)
{
    for (auto * n = static_cast<Hashtable::Node *>(node)->next; n != nullptr; n = n->next) {
        if (n->hashCode == hash) return n;
    }
    return nullptr;
}

static void aggregation_overflow()
{
    throw std::runtime_error("numeric field overflow in an aggregate");
}

static bool isInteger(Sql::SqlType type)
{
    using ID = Sql::SqlType::TypeID;
    return type.typeID == ID::IntegerID || type.typeID == ID::LongIntegerID || type.typeID == ID::NumericID;
}

Aggregation::Aggregation(const logical_operator_t & logicalOperator, std::unique_ptr<Operator> input, QueryContext & queryContext) :
        UnaryOperator(logicalOperator, std::move(input), queryContext)
{
    auto & context = _codeGen.getLLVMContext();
    llvm::Type * i64 = llvm::Type::getInt64Ty(context);
    std::vector<llvm::Type *> members;
    for (auto & key : logical()._keys) {
        _keyTypes.push_back(key.input->sqlType);
        members.push_back(Sql::toLLVMTy(key.input->sqlType));
    }
    // per aggregate: { count, state }
    for (auto & aggregate : logical()._aggregates) {
        Sql::SqlType input = aggregate.input != nullptr ? Sql::toNotNullableTy(aggregate.input->sqlType) : Sql::getIntegerTy();
        _inputTypes.push_back(input);
        llvm::Type * state = i64;
        if (aggregate.function == Function::Sum || aggregate.function == Function::Avg) {
            if (input.typeID == Sql::SqlType::TypeID::ArrayID) state = llvm::PointerType::getUnqual(context);
            else if (input.typeID == Sql::SqlType::TypeID::DoubleID) state = llvm::Type::getDoubleTy(context);
            else state = llvm::Type::getInt128Ty(context);
        } else if (aggregate.function == Function::Min || aggregate.function == Function::Max) {
            state = Sql::toLLVMTy(input);
        }
        members.push_back(llvm::StructType::get(context, {i64, state}));
    }
    _groupType = llvm::StructType::create(context, members, "aggregationGroup");
    _groupSize = _codeGen.getCurrentModuleGen().getDataLayout().getTypeAllocSize(_groupType);
}

Aggregation::~Aggregation()
{ }

const Logical::Aggregation & Aggregation::logical() const
{
    return static_cast<const Logical::Aggregation &>(_logicalOperator);
}

llvm::Value * Aggregation::field(cg_voidptr_t group, unsigned index)
{
    return _codeGen->CreateStructGEP(_groupType, group.getValue(), index);
}

void Aggregation::produce()
{
    auto owned = std::make_unique<State>();
    _state = owned.get();
    _context.executionContext.acquireResource(std::move(owned));
    _state->arena = createQueryArena();
    for (auto & type : _inputTypes) _state->elementTypes.push_back(type.typeID == Sql::SqlType::TypeID::ArrayID ? Sql::getElementTy(type) : type);

    llvm::Value * size = cg_size_t(_groupSize).getValue();
    llvm::Value * zero = _codeGen->getInt8(0);
    if (logical()._keys.empty()) {
        // one group, in the entry block, cleared at each execution (also in a loop)
        _singleGroup = Sql::genEntryAlloca(_groupType);
        LLVMCompat::createMemSet(_codeGen.getIRBuilder(), _singleGroup, zero, size);
        child->produce();
        emitGroup(cg_voidptr_t(_singleGroup));
    } else {
        _hashTable = genHashtableCreateCall();
        child->produce();
        genHashtableIter(_hashTable, [&](cg_voidptr_t node) { emitGroup(genHashtableGetUserDataPtr(node)); });
        genHashtableFreeCall(_hashTable);
    }
    llvm::FunctionType * resetTy = llvm::FunctionType::get(_codeGen->getVoidTy(), {cg_voidptr_t::getType()}, false);
    _codeGen.CreateCall(&aggregation_reset, resetTy, {cg_voidptr_t::fromRawPointer(_state).getValue()});
}

Sql::value_op_t Aggregation::keyValue(const Sql::Value & value, Sql::SqlType type) const
{
    if (type.nullable && !Sql::isNullable(value)) return Sql::NullableValue::create(value, cg_bool_t(false));
    return value.clone();
}

cg_bool_t Aggregation::keysEqual(cg_voidptr_t group, const std::vector<Sql::value_op_t> & keys)
{
    llvm::Value * equal = _codeGen->getInt1(true);
    for (size_t k = 0; k < keys.size(); ++k) {
        Sql::value_op_t stored = Sql::Value::load(field(group, static_cast<unsigned>(k)), _keyTypes[k]);
        if (!_keyTypes[k].nullable) {
            equal = _codeGen->CreateAnd(equal, stored->equals(*keys[k]));
            continue;
        }
        // NULL equals NULL here; the values compare only if neither is NULL
        auto & a = static_cast<const Sql::NullableValue &>(*stored);
        auto & b = static_cast<const Sql::NullableValue &>(*keys[k]);
        cg_bool_t bothNull = a.isNull() && b.isNull();
        PhiNode<llvm::Value> result(bothNull, "keyEqual");
        IfGen bothValues(!a.isNull() && !b.isNull());
        {
            result = a.getValue().equals(b.getValue());
        }
        bothValues.EndIf();
        equal = _codeGen->CreateAnd(equal, result.get());
    }
    return cg_bool_t(equal);
}

void Aggregation::consume(const iu_value_mapping_t & values, const Operator & src)
{
    auto & funcGen = _codeGen.getCurrentFunctionGen();
    if (logical()._keys.empty()) {
        update(cg_voidptr_t(_singleGroup), values);
        return;
    }
    std::vector<Sql::value_op_t> keys;
    for (size_t k = 0; k < logical()._keys.size(); ++k) keys.push_back(keyValue(*values.at(logical()._keys[k].input), _keyTypes[k]));
    cg_hash_t hash = keys[0]->hash();
    for (size_t k = 1; k < keys.size(); ++k) hash = Sql::genHashCombine(hash, keys[k]->hash());

    // the group of the keys: a node of the hash, whose keys are equal
    llvm::Type * ptrTy = cg_voidptr_t::getType();
    llvm::Value * found = Sql::genEntryAlloca(ptrTy);
    llvm::Value * null = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy));
    _codeGen->CreateStore(null, found);
    cg_voidptr_t first = genHashtableLookupCall(_hashTable, hash);
    LoopGen chain(funcGen, !first.isNullPtr(), {{"node", first}});
    cg_voidptr_t node(chain.getLoopVar(0));
    cg_voidptr_t next;
    {
        LoopBodyGen body(chain);
        IfGen match(funcGen, keysEqual(genHashtableGetUserDataPtr(node), keys), {{"next", cg_voidptr_t(null)}});
        {
            _codeGen->CreateStore(node.getValue(), found);
            match.setVar(0, null);
        }
        match.Else();
        {
            llvm::FunctionType * nextTy = llvm::FunctionType::get(ptrTy, {ptrTy, _codeGen->getInt64Ty()}, false);
            match.setVar(0, _codeGen.CreateCall(&aggregation_next, nextTy, {node.getValue(), hash.getValue()}));
        }
        match.EndIf();
        next = cg_voidptr_t(match.getResult(0));
    }
    chain.loopDone(!next.isNullPtr(), {next});

    cg_voidptr_t foundNode(_codeGen->CreateLoad(ptrTy, found));
    IfGen absent(funcGen, foundNode.isNullPtr(), {{"group", cg_voidptr_t(null)}});
    {
        // a new group: its keys, and the aggregates of no values
        cg_voidptr_t group = genHashtableGetUserDataPtr(genHashtableInsertCall(_hashTable, hash, cg_size_t(_groupSize)));
        LLVMCompat::createMemSet(_codeGen.getIRBuilder(), group.getValue(), _codeGen->getInt8(0), cg_size_t(_groupSize).getValue());
        for (size_t k = 0; k < keys.size(); ++k) keys[k]->store(field(group, static_cast<unsigned>(k)));
        absent.setVar(0, group.getValue());
    }
    absent.Else();
    {
        absent.setVar(0, genHashtableGetUserDataPtr(foundNode).getValue());
    }
    absent.EndIf();
    update(cg_voidptr_t(absent.getResult(0)), values);
}

void Aggregation::update(cg_voidptr_t group, const iu_value_mapping_t & values)
{
    auto & context = _codeGen.getLLVMContext();
    llvm::Type * i64 = llvm::Type::getInt64Ty(context);
    const size_t keyCount = logical()._keys.size();
    for (size_t a = 0; a < logical()._aggregates.size(); ++a) {
        const auto & aggregate = logical()._aggregates[a];
        llvm::Value * entry = field(group, static_cast<unsigned>(keyCount + a));
        llvm::StructType * entryTy = llvm::cast<llvm::StructType>(_groupType->getElementType(static_cast<unsigned>(keyCount + a)));
        llvm::Value * countPtr = _codeGen->CreateStructGEP(entryTy, entry, 0);
        llvm::Value * statePtr = _codeGen->CreateStructGEP(entryTy, entry, 1);
        auto increment = [&]() {
            llvm::Value * count = _codeGen->CreateLoad(i64, countPtr);
            _codeGen->CreateStore(_codeGen->CreateAdd(count, _codeGen->getInt64(1)), countPtr);
        };
        if (aggregate.function == Function::CountAll) {
            increment();
            continue;
        }
        // NULLs are skipped
        const Sql::Value & input = *values.at(aggregate.input);
        std::unique_ptr<IfGen> present;
        const Sql::Value * value = &input;
        if (Sql::isNullable(input)) {
            auto & nullable = static_cast<const Sql::NullableValue &>(input);
            present = std::make_unique<IfGen>(!nullable.isNull());
            value = &nullable.getValue();
        }
        const Sql::SqlType type = _inputTypes[a];
        switch (aggregate.function) {
            case Function::Count:
                break;
            case Function::Sum:
            case Function::Avg:
                if (type.typeID == Sql::SqlType::TypeID::ArrayID) {
                    llvm::FunctionType * addTy = llvm::FunctionType::get(_codeGen->getVoidTy(),
                            {cg_voidptr_t::getType(), _codeGen->getInt32Ty(), cg_voidptr_t::getType(), cg_voidptr_t::getType()}, false);
                    _codeGen.CreateCall(&aggregation_array_add, addTy, {cg_voidptr_t::fromRawPointer(_state).getValue(),
                            _codeGen->getInt32(static_cast<uint32_t>(a)), statePtr, static_cast<const Sql::Array &>(*value).spill()});
                } else if (type.typeID == Sql::SqlType::TypeID::DoubleID) {
                    llvm::Value * sum = _codeGen->CreateLoad(_codeGen->getDoubleTy(), statePtr);
                    _codeGen->CreateStore(_codeGen->CreateFAdd(sum, value->getLLVMValue()), statePtr);
                } else {
                    llvm::Type * i128 = llvm::Type::getInt128Ty(context);
                    llvm::Value * sum = _codeGen->CreateLoad(i128, statePtr);
                    _codeGen->CreateStore(_codeGen->CreateAdd(sum, _codeGen->CreateSExt(value->getLLVMValue(), i128)), statePtr);
                }
                break;
            case Function::Min:
            case Function::Max: {
                // the first value, or one that is less (greater)
                llvm::Value * count = _codeGen->CreateLoad(i64, countPtr);
                IfGen first(_codeGen->CreateICmpEQ(count, _codeGen->getInt64(0)));
                {
                    value->store(statePtr);
                }
                first.Else();
                {
                    Sql::value_op_t best = Sql::Value::load(statePtr, type);
                    IfGen better(value->compare(*best, aggregate.function == Function::Min ? Sql::ComparisonMode::less : Sql::ComparisonMode::gtr));
                    {
                        value->store(statePtr);
                    }
                    better.EndIf();
                }
                first.EndIf();
                break;
            }
            case Function::CountAll:
                break;
        }
        increment();
        if (present) present->EndIf();
    }
}

void Aggregation::emitGroup(cg_voidptr_t group)
{
    auto & context = _codeGen.getLLVMContext();
    llvm::Type * i64 = llvm::Type::getInt64Ty(context);
    iu_value_mapping_t values;
    std::vector<Sql::value_op_t> owned;
    const size_t keyCount = logical()._keys.size();
    for (size_t k = 0; k < keyCount; ++k) {
        owned.push_back(Sql::Value::load(field(group, static_cast<unsigned>(k)), _keyTypes[k]));
        values[logical()._keys[k].output] = owned.back().get();
    }
    for (size_t a = 0; a < logical()._aggregates.size(); ++a) {
        const auto & aggregate = logical()._aggregates[a];
        const Sql::SqlType input = _inputTypes[a];
        const Sql::SqlType result = aggregate.output->sqlType;
        const Sql::SqlType plain = Sql::toNotNullableTy(result);
        llvm::Value * entry = field(group, static_cast<unsigned>(keyCount + a));
        llvm::StructType * entryTy = llvm::cast<llvm::StructType>(_groupType->getElementType(static_cast<unsigned>(keyCount + a)));
        llvm::Value * count = _codeGen->CreateLoad(i64, _codeGen->CreateStructGEP(entryTy, entry, 0));
        llvm::Value * statePtr = _codeGen->CreateStructGEP(entryTy, entry, 1);
        const bool average = aggregate.function == Function::Avg;
        Sql::value_op_t value;
        if (aggregate.function == Function::CountAll || aggregate.function == Function::Count) {
            value = Sql::Value::fromRawValues({count}, plain);
        } else if (aggregate.function == Function::Min || aggregate.function == Function::Max) {
            // bigints become numerics of the same representation
            value = Sql::Value::load(statePtr, isInteger(plain) && isInteger(input) ? plain : input);
        } else if (input.typeID == Sql::SqlType::TypeID::ArrayID) {
            llvm::Value * slot = Sql::genEntryAlloca(Sql::toLLVMTy(plain));
            llvm::Value * sum = _codeGen->CreateLoad(cg_voidptr_t::getType(), statePtr);
            llvm::FunctionType * resultTy = llvm::FunctionType::get(_codeGen->getVoidTy(),
                    {cg_voidptr_t::getType(), cg_voidptr_t::getType(), i64, _codeGen->getInt1Ty(), cg_voidptr_t::getType()}, false);
            _codeGen.CreateCall(&aggregation_array_result, resultTy, {cg_voidptr_t::fromRawPointer(_state).getValue(), sum, count,
                    _codeGen->getInt1(average), slot});
            value = Sql::Value::load(slot, plain);
        } else if (input.typeID == Sql::SqlType::TypeID::DoubleID) {
            llvm::Value * sum = _codeGen->CreateLoad(_codeGen->getDoubleTy(), statePtr);
            if (average) {
                // a group without values is NULL: no division by zero
                llvm::Value * divisor = _codeGen->CreateUIToFP(_codeGen->CreateSelect(_codeGen->CreateICmpEQ(count, _codeGen->getInt64(0)),
                                                                                      _codeGen->getInt64(1), count), _codeGen->getDoubleTy());
                sum = _codeGen->CreateFDiv(sum, divisor);
            }
            value = Sql::Value::fromRawValues({sum}, plain);
        } else {
            llvm::Type * i128 = llvm::Type::getInt128Ty(context);
            llvm::Value * sum = _codeGen->CreateLoad(i128, statePtr);
            if (average) {
                llvm::Value * divisor = _codeGen->CreateSelect(_codeGen->CreateICmpEQ(count, _codeGen->getInt64(0)), _codeGen->getInt64(1), count);
                sum = _codeGen->CreateSDiv(sum, _codeGen->CreateZExt(divisor, i128));
            }
            // the result in 64 bits (an integer average in 32)
            llvm::Type * resultTy = Sql::toLLVMTy(plain);
            const unsigned bits = resultTy->getIntegerBitWidth();
            llvm::Value * low = _codeGen->CreateTrunc(sum, resultTy);
            IfGen overflow(_codeGen->CreateICmpNE(_codeGen->CreateSExt(low, i128), sum));
            {
                llvm::FunctionType * overflowTy = llvm::FunctionType::get(_codeGen->getVoidTy(), {}, false);
                _codeGen.CreateCall(&aggregation_overflow, overflowTy);
            }
            overflow.EndIf();
            (void) bits;
            value = Sql::Value::fromRawValues({low}, plain);
        }
        if (result.nullable) value = Sql::NullableValue::create(std::move(value), cg_bool_t(_codeGen->CreateICmpEQ(count, _codeGen->getInt64(0))));
        owned.push_back(std::move(value));
        values[aggregate.output] = owned.back().get();
    }
    emit(values);
}

} // end namespace Physical
} // end namespace Algebra
