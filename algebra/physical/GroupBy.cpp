
#include "GroupBy.hpp"

#include <llvm/IR/DerivedTypes.h>

#include "utils.hpp"
#include "Hashtable.hpp"
#include "PhiNode.hpp"
#include "LegacyTypes.hpp"
#include "sql/SqlTuple.hpp"
#include "sql/SqlType.hpp"
#include "sql/SqlUtils.hpp"

using namespace Algebra::Physical::Expressions;
using namespace Sql;

// source: https://llvm.org/svn/llvm-project/llvm/trunk/examples/ExceptionDemo/ExceptionDemo.cpp
/// Create an alloca instruction in the entry block of
/// the parent function.  This is used for mutable variables etc.
/// @param function parent instance
/// @param varName stack variable name
/// @param type stack variable type
/// @param initWith optional constant initialization value
/// @returns AllocaInst instance
static llvm::AllocaInst *createEntryBlockAlloca(llvm::Function &function,
                                                const std::string &varName,
                                                llvm::Type *type,
                                                llvm::Constant *initWith = 0) {
    llvm::BasicBlock &block = function.getEntryBlock();
    llvm::IRBuilder<> tmp(&block, block.begin());
    llvm::AllocaInst *ret = tmp.CreateAlloca(type, 0, varName);

    if (initWith)
        tmp.CreateStore(initWith, ret);

    return(ret);
}

namespace Algebra {
namespace Physical {

namespace Aggregations {

//-----------------------------------------------------------------------------
// Aggregator

Aggregator::Aggregator(QueryContext & context, iu_p_t produced) :
        _context(context),
        _produced(produced)
{ }

Aggregator::~Aggregator()
{ }

iu_p_t Aggregator::getProduced()
{
    return _produced;
}

//-----------------------------------------------------------------------------
// Keep

Keep::Keep(QueryContext & context, iu_p_t produced, iu_p_t keep) :
        Aggregator(context, produced),
        _keep(keep)
{ }

Keep::~Keep()
{ }

void Keep::consumeFirst(const iu_value_mapping_t & values, llvm::Value * entryPtr)
{
    Sql::Value & value = *values.at(_keep);
    value.store(entryPtr);
}

void Keep::consumeNext(const iu_value_mapping_t & values, llvm::Value * entryPtr)
{
    // NOP
}

Sql::SqlType Keep::getResultType()
{
    return _keep->sqlType;
}

Sql::value_op_t Keep::getResult(llvm::Value * entryPtr)
{
    return Sql::Value::load(entryPtr, _keep->sqlType);
}

llvm::Type * Keep::getEntryType()
{
    return toLLVMTy(_keep->sqlType);
}

//-----------------------------------------------------------------------------
// Sum

Sum::Sum(QueryContext & context, iu_p_t produced, Expressions::exp_op_t exp) :
        Aggregator(context, produced),
        _expression(std::move(exp))
{ }

Sum::~Sum()
{ }

void Sum::consumeFirst(const iu_value_mapping_t & values, llvm::Value * entryPtr)
{
    value_op_t sqlValue = _expression->evaluate(values);
    sqlValue->store(entryPtr);
    value_op_t a = Sql::Value::load(entryPtr, sqlValue->type);
}

void Sum::consumeNext(const iu_value_mapping_t & values, llvm::Value * entryPtr)
{
    value_op_t a = Sql::Value::load(entryPtr, _expression->getType());
    value_op_t b = _expression->evaluate(values);
    value_op_t acc = Sql::Operators::sqlAdd(*a, *b);
    acc->store(entryPtr);
}

Sql::SqlType Sum::getResultType()
{
    return _expression->getType();
}

Sql::value_op_t Sum::getResult(llvm::Value * entryPtr)
{
    return Sql::Value::load(entryPtr, _expression->getType());
}

llvm::Type * Sum::getEntryType()
{
    return toLLVMTy(getResultType());
}

//-----------------------------------------------------------------------------
// Avg

Avg::Avg(QueryContext & context, iu_p_t produced, Expressions::exp_op_t exp) :
        Aggregator(context, produced),
        _expression(std::move(exp))
{
    createEntryType();
}

Avg::~Avg()
{ }

void Avg::consumeFirst(const iu_value_mapping_t & values, llvm::Value * entryPtr)
{
    auto & codeGen = _context.codeGen;

    // get accumulator pointer
    llvm::Value * accPtr = codeGen->CreateStructGEP(_entryTy, entryPtr, 0);
    value_op_t first = _expression->evaluate(values);
    first->store(accPtr);

    // set-up counter
    Sql::SqlType type = _expression->getType();
    Sql::Numeric one(type, numericShifts[type.numericLayout.precision]);
    llvm::Value * countPtr = codeGen->CreateStructGEP(_entryTy, entryPtr, 1);
    one.store(countPtr);
}

void Avg::consumeNext(const iu_value_mapping_t & values, llvm::Value * entryPtr)
{
    auto & codeGen = _context.codeGen;

    // get accumulator pointer
    llvm::Value * accPtr = codeGen->CreateStructGEP(_entryTy, entryPtr, 0);
    value_op_t acc = Sql::Value::load(accPtr, _expression->getType());
    value_op_t current = _expression->evaluate(values);
    value_op_t newAcc = Sql::Operators::sqlAdd(*acc, *current);
    newAcc->store(accPtr);

    // update counter
    Sql::SqlType type = _expression->getType();
    Sql::Numeric one(type, numericShifts[type.numericLayout.precision]);
    llvm::Value * countPtr = codeGen->CreateStructGEP(_entryTy, entryPtr, 1);
    value_op_t count = Sql::Value::load(countPtr, type);
    value_op_t newCount = Sql::Operators::sqlAdd(one, *count);
    newCount->store(countPtr);
}

Sql::SqlType Avg::getResultType()
{
    return _expression->getType();
}

Sql::value_op_t Avg::getResult(llvm::Value * entryPtr)
{
    auto & codeGen = _context.codeGen;
    Sql::SqlType type = _expression->getType();

    llvm::Value * accPtr = codeGen->CreateStructGEP(_entryTy, entryPtr, 0);
    value_op_t acc = Sql::Value::load(accPtr, type);

    llvm::Value * countPtr = codeGen->CreateStructGEP(_entryTy, entryPtr, 1);
    value_op_t count = Sql::Value::load(countPtr, type);

    // calculate average
    return Sql::Operators::sqlDiv(*acc, *count);
}

llvm::Type * Avg::getEntryType()
{
    return _entryTy;
}

void Avg::createEntryType()
{
    llvm::Type * valueTy = toLLVMTy(getResultType());
    llvm::StructType * entryTy = llvm::StructType::create(_context.codeGen.getLLVMContext(), "avgEntryTy");

    // layout: { accumulator, counter }
    entryTy->setBody({ valueTy, valueTy });
    _entryTy = entryTy;
}

//-----------------------------------------------------------------------------
// CountAll

CountAll::CountAll(QueryContext & context, iu_p_t produced) :
        Aggregator(context, produced)
{ }

CountAll::~CountAll()
{ }

void CountAll::consumeFirst(const iu_value_mapping_t & values, llvm::Value * entryPtr)
{
    Sql::Integer count(1);
    count.store(entryPtr);
}

void CountAll::consumeNext(const iu_value_mapping_t & values, llvm::Value * entryPtr)
{
    Sql::Integer one(1);
    value_op_t count = Sql::Value::load(entryPtr, Sql::getIntegerTy());
    value_op_t newCount = Sql::Operators::sqlAdd(one, *count);
    newCount->store(entryPtr);
}

Sql::SqlType CountAll::getResultType()
{
    return Sql::getIntegerTy();
}

Sql::value_op_t CountAll::getResult(llvm::Value * entryPtr)
{
    return Sql::Value::load(entryPtr, Sql::getIntegerTy());
}

llvm::Type * CountAll::getEntryType()
{
    return toLLVMTy(getResultType());
}

//-----------------------------------------------------------------------------
// Min

Min::Min(QueryContext & context, iu_p_t produced, Expressions::exp_op_t exp) :
        Aggregator(context, produced),
        _expression(std::move(exp))
{ }

Min::~Min()
{ }

void Min::consumeFirst(const iu_value_mapping_t & values, llvm::Value * entryPtr)
{
    value_op_t sqlValue = _expression->evaluate(values);
    sqlValue->store(entryPtr);
    value_op_t a = Sql::Value::load(entryPtr, sqlValue->type);
}

void Min::consumeNext(const iu_value_mapping_t & values, llvm::Value * entryPtr)
{
    value_op_t b = _expression->evaluate(values);
    value_op_t a = Sql::Value::load(entryPtr, _expression->getType());

    auto action = [&](const Sql::Value & lhs, const Sql::Value & rhs) {
        // new min value?
        cg_bool_t result = lhs.compare(rhs, ComparisonMode::less);

        PhiNode<Sql::Value> min(rhs, "min");
        IfGen check(result);
        {
            min = lhs;
        }
        check.EndIf();

        return min.get();
    };

    value_op_t min = Sql::Utils::nullHandler(action, *a, *b, _expression->getType());
    min->store(entryPtr);
}

Sql::SqlType Min::getResultType()
{
    return _expression->getType();
}

Sql::value_op_t Min::getResult(llvm::Value * entryPtr)
{
    return Sql::Value::load(entryPtr, _expression->getType());
}

llvm::Type * Min::getEntryType()
{
    return toLLVMTy(getResultType());
}

} // end namespace Aggregations

//-----------------------------------------------------------------------------
// GroupBy

using namespace Aggregations;

GroupBy::GroupBy(std::unique_ptr<Operator> input, const iu_set_t & required,
                 std::vector<std::unique_ptr<Aggregations::Aggregator>> aggregations) :
        UnaryOperator(std::move(input), required),
        _aggregations(std::move(aggregations))
{
    createGroupType();

    // collect keep aggregation functions
    for (auto & aggregator : _aggregations) {
        if (Keep * keep = dynamic_cast<Keep *>(aggregator.get())) {
            _keepSet.insert(keep->getKeepIU());
        }
    }

    // set operation mode
    // (a hash table is only used if at least one keep aggregation function is involved)
    _keepMode = !_keepSet.empty();

    // calculate _groupSize
    auto & dataLayout = codeGen.getCurrentModuleGen().getDataLayout();
    _groupSize = dataLayout.getTypeAllocSize(_groupType);
}

GroupBy::~GroupBy()
{ }

void GroupBy::produce()
{
    if (_keepMode) {
        _hashTable = genHashtableCreateCall();

        child->produce();

        // pass each group to the parent operator
        genHashtableIter(_hashTable, [&](cg_voidptr_t nodePtr) {
            // get payload pointer
            cg_voidptr_t groupPtr = genHashtableGetUserDataPtr(nodePtr);
            auto result = getResult(groupPtr);
            parent->consume(result.second, *this);
        });

        genHashtableFreeCall(_hashTable);
    } else {
        _emptyStackVar = createEntryBlockAlloca(
                codeGen.getCurrentFunction(), "isEmpty", codeGen->getInt1Ty(), codeGen->getInt1(true)
        );

        // reserve memory for exactly one group
        _singleGroup = Functions::genMallocCall(cg_size_t(_groupSize));

        child->produce();

        // pass the group to the parent if it is not empty
        cg_bool_t isEmpty( codeGen->CreateLoad(_emptyStackVar) );
        IfGen notEmptyCheck(!isEmpty);
        {
            auto result = getResult(_singleGroup);
            parent->consume(result.second, *this);
        }
        notEmptyCheck.EndIf();

        Functions::genFreeCall(_singleGroup);
    }
}

void GroupBy::createGroupType()
{
    // contruct a struct with an entry for each aggregation function
    std::vector<llvm::Type *> members;
    for (auto & aggregator : _aggregations) {
        members.push_back(aggregator->getEntryType());
    }

    llvm::StructType * groupTy = llvm::StructType::create(codeGen->getContext(), "groupTy");
    groupTy->setBody(members);

    _groupType = groupTy;
}

cg_hash_t GroupBy::currentHash(const iu_value_mapping_t & values)
{
    assert(!_keepSet.empty());
    auto it = _keepSet.begin();

    // calculate the initial hash
    cg_hash_t hash = values.at(*it)->hash();

    // combine it with the remaining attribute's hash
    for (++it; it != _keepSet.end(); ++it) {
        hash = genHashCombine(hash, values.at(*it)->hash());
    }

    return hash;
}

void GroupBy::initializeGroup(cg_voidptr_t groupRawPtr, const iu_value_mapping_t & values)
{
    llvm::Value * groupPtr = codeGen->CreateBitCast(groupRawPtr, llvm::PointerType::getUnqual(_groupType));

    unsigned i = 0;
    for (auto & aggregator : _aggregations) {
        llvm::Value * entryPtr = codeGen->CreateStructGEP(_groupType, groupPtr, i);
        aggregator->consumeFirst(values, entryPtr);

        i += 1;
    }
}

void GroupBy::aggregateInto(cg_voidptr_t groupRawPtr, const iu_value_mapping_t & values)
{
    llvm::Value * groupPtr = codeGen->CreateBitCast(groupRawPtr, llvm::PointerType::getUnqual(_groupType));

    unsigned i = 0;
    for (auto & aggregator : _aggregations) {
        llvm::Value * entryPtr = codeGen->CreateStructGEP(_groupType, groupPtr, i);
        aggregator->consumeNext(values, entryPtr);

        i += 1;
    }
}

std::pair<std::vector<value_op_t>, iu_value_mapping_t> GroupBy::getResult(cg_voidptr_t groupRawPtr)
{
    std::pair<std::vector<value_op_t>, iu_value_mapping_t> result;

    llvm::Value * groupPtr = codeGen->CreateBitCast(groupRawPtr, llvm::PointerType::getUnqual(_groupType));

    // finalize each aggregation function and collect the results
    unsigned i = 0;
    for (auto & aggregator : _aggregations) {
        llvm::Value * entryPtr = codeGen->CreateStructGEP(_groupType, groupPtr, i);

        value_op_t resultValue = aggregator->getResult(entryPtr);
        iu_p_t iu = aggregator->getProduced();

        result.second.insert(std::make_pair(iu, resultValue.get()));
        result.first.push_back(std::move(resultValue));

        i += 1;
    }

    return result;
}

void GroupBy::consume(const iu_value_mapping_t & values, const Operator & src)
{
    if (_keepMode) {
        // lookup aggregation group
        cg_hash_t hash = currentHash(values);
        cg_voidptr_t node = genHashtableLookupCall(_hashTable, hash);

        IfGen isNull(node.isNullPtr());
        {
            // create a new group and initialize it
            cg_size_t groupSize(_groupSize);
            cg_voidptr_t newNode = genHashtableInsertCall(_hashTable, hash, groupSize);
            cg_voidptr_t group = genHashtableGetUserDataPtr(newNode);
            initializeGroup(group, values);
        }
        isNull.Else();
        {
            // update the current aggregation group
            cg_voidptr_t group = genHashtableGetUserDataPtr(node);
            aggregateInto(group, values);
        }
        isNull.EndIf();
    } else {
        cg_bool_t isEmpty( codeGen->CreateLoad(_emptyStackVar) );
        IfGen emptyCheck(isEmpty);
        {
            // create a new group and initialize it
            codeGen->CreateStore(codeGen->getInt1(false), _emptyStackVar);
            initializeGroup(_singleGroup, values);
        }
        emptyCheck.Else();
        {
            // update the current aggregation group
            aggregateInto(_singleGroup, values);
        }
        emptyCheck.EndIf();
    }
}

} // end namespace Physical
} // end namespace Algebra
