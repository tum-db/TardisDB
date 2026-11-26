#include "algebra/physical/Fixpoint.hpp"

#include "codegen/LLVMCompat.hpp"

#include <stdexcept>

#include "sql/SqlValues.hpp"

namespace Algebra {
namespace Physical {

/// \returns The address of a new row at the end of the buffer
static void * fixpoint_append(void * buffer, uint64_t rowSize)
{
    auto * b = static_cast<FixpointBuffer *>(buffer);
    b->data.resize((b->rows + 1) * rowSize);
    return b->data.data() + (b->rows++) * rowSize;
}

static uint64_t fixpoint_rows(void * buffer)
{
    return static_cast<FixpointBuffer *>(buffer)->rows;
}

static void * fixpoint_data(void * buffer)
{
    return static_cast<FixpointBuffer *>(buffer)->data.data();
}

/// Empties the buffers (at the start of an execution)
static void fixpoint_reset(void * state)
{
    auto * s = static_cast<FixpointState *>(state);
    for (FixpointBuffer * b : {&s->working, &s->next, &s->result}) {
        b->data.clear();
        b->rows = 0;
    }
}

/// The rows of the step become the working table, and the next step starts without rows
static void fixpoint_advance(void * state)
{
    auto * s = static_cast<FixpointState *>(state);
    std::swap(s->working, s->next);
    s->next.data.clear();
    s->next.rows = 0;
}

llvm::StructType * FixpointState::rowType() const
{
    auto & codeGen = getThreadLocalCodeGen();
    std::vector<llvm::Type *> members;
    for (auto & type : types) members.push_back(Sql::toLLVMTy(type));
    return llvm::StructType::get(codeGen.getLLVMContext(), members);
}

static llvm::Value * bufferPointer(FixpointBuffer & buffer)
{
    return cg_voidptr_t::fromRawPointer(&buffer).getValue();
}

static llvm::Value * callRows(CodeGen & codeGen, FixpointBuffer & buffer)
{
    llvm::FunctionType * type = llvm::FunctionType::get(codeGen->getInt64Ty(), {cg_voidptr_t::getType()}, false);
    return codeGen.CreateCall(&fixpoint_rows, type, {bufferPointer(buffer)});
}

static llvm::Value * callData(CodeGen & codeGen, FixpointBuffer & buffer)
{
    llvm::FunctionType * type = llvm::FunctionType::get(cg_voidptr_t::getType(), {cg_voidptr_t::getType()}, false);
    return codeGen.CreateCall(&fixpoint_data, type, {bufferPointer(buffer)});
}

/// Generates a loop over the rows of a buffer (which grows in the loop no more), with the address of each row
static void forEachRow(CodeGen & codeGen, FixpointState & state, FixpointBuffer & buffer, const std::function<void(llvm::Value *)> & body)
{
    auto & funcGen = codeGen.getCurrentFunctionGen();
    llvm::Value * rows = callRows(codeGen, buffer);
    llvm::Value * data = callData(codeGen, buffer);
    LoopGen loop(funcGen, codeGen->CreateICmpULT(codeGen->getInt64(0), rows), {{"row", cg_size_t(0ul)}});
    cg_size_t row(loop.getLoopVar(0));
    {
        LoopBodyGen bodyGen(loop);
        llvm::Value * offset = codeGen->CreateMul(row.getValue(), codeGen->getInt64(state.rowSize));
        body(codeGen->CreateGEP(codeGen->getInt8Ty(), data, offset));
    }
    cg_size_t next = row + 1ul;
    loop.loopDone(codeGen->CreateICmpULT(next.getValue(), rows), {next});
}

Fixpoint::Fixpoint(const logical_operator_t & logicalOperator, std::unique_ptr<Operator> initial, std::unique_ptr<Operator> step,
                   FixpointState & state, QueryContext & queryContext) :
        BinaryOperator(logicalOperator, std::move(initial), std::move(step), queryContext),
        _state(state)
{ }

Fixpoint::~Fixpoint()
{ }

const Logical::Fixpoint & Fixpoint::logical() const
{
    return static_cast<const Logical::Fixpoint &>(_logicalOperator);
}

void Fixpoint::produce()
{
    auto & funcGen = _codeGen.getCurrentFunctionGen();
    llvm::Value * statePtr = cg_voidptr_t::fromRawPointer(&_state).getValue();
    llvm::FunctionType * stateTy = llvm::FunctionType::get(_codeGen->getVoidTy(), {cg_voidptr_t::getType()}, false);
    _codeGen.CreateCall(&fixpoint_reset, stateTy, {statePtr});

    // the initial query fills the working table; each step reads it and fills the next one, until a step adds no rows
    _leftChild->produce();
    LoopGen steps(funcGen, _codeGen->CreateICmpULT(_codeGen->getInt64(0), callRows(_codeGen, _state.working)), {{"step", cg_size_t(0ul)}});
    cg_size_t step(steps.getLoopVar(0));
    {
        LoopBodyGen bodyGen(steps);
        _rightChild->produce();
        _codeGen.CreateCall(&fixpoint_advance, stateTy, {statePtr});
    }
    cg_size_t nextStep = step + 1ul;
    steps.loopDone(_codeGen->CreateICmpULT(_codeGen->getInt64(0), callRows(_codeGen, _state.working)), {nextStep});

    // all rows
    llvm::StructType * rowType = _state.rowType();
    forEachRow(_codeGen, _state, _state.result, [&](llvm::Value * row) {
        iu_value_mapping_t values;
        std::vector<Sql::value_op_t> owned;
        for (size_t c = 0; c < logical()._outputs.size(); ++c) {
            owned.push_back(Sql::Value::load(_codeGen->CreateStructGEP(rowType, row, static_cast<unsigned>(c)), _state.types[c]));
            values[logical()._outputs[c]] = owned.back().get();
        }
        emit(values);
    });
}

void Fixpoint::append(FixpointBuffer & buffer, const std::vector<iu_p_t> & columns, const iu_value_mapping_t & values)
{
    llvm::StructType * rowType = _state.rowType();
    llvm::FunctionType * appendTy = llvm::FunctionType::get(cg_voidptr_t::getType(), {cg_voidptr_t::getType(), _codeGen->getInt64Ty()}, false);
    llvm::Value * row = _codeGen.CreateCall(&fixpoint_append, appendTy, {bufferPointer(buffer), _codeGen->getInt64(_state.rowSize)});
    for (size_t c = 0; c < columns.size(); ++c) {
        const Sql::Value & value = *values.at(columns[c]);
        Sql::value_op_t stored = (_state.types[c].nullable && !Sql::isNullable(value)) ? Sql::NullableValue::create(value, cg_bool_t(false))
                                                                                      : value.clone();
        stored->store(_codeGen->CreateStructGEP(rowType, row, static_cast<unsigned>(c)));
    }
}

void Fixpoint::consumeLeft(const iu_value_mapping_t & values)
{
    append(_state.working, logical()._initialColumns, values);
    append(_state.result, logical()._initialColumns, values);
}

void Fixpoint::consumeRight(const iu_value_mapping_t & values)
{
    append(_state.next, logical()._stepColumns, values);
    append(_state.result, logical()._stepColumns, values);
}

WorkingTableScan::WorkingTableScan(const logical_operator_t & logicalOperator, FixpointState & state, QueryContext & queryContext) :
        NullaryOperator(logicalOperator, queryContext),
        _state(state)
{ }

WorkingTableScan::~WorkingTableScan()
{ }

void WorkingTableScan::produce()
{
    llvm::StructType * rowType = _state.rowType();
    // the columns by their names
    std::vector<std::pair<iu_p_t, unsigned>> columns;
    for (iu_p_t iu : getRequired()) {
        const std::string & name = iu->columnInformation->columnName;
        auto it = std::find(_state.names.begin(), _state.names.end(), name);
        if (it == _state.names.end()) throw std::runtime_error("the working table of a recursive query has no column " + name);
        columns.emplace_back(iu, static_cast<unsigned>(it - _state.names.begin()));
    }
    forEachRow(_codeGen, _state, _state.working, [&](llvm::Value * row) {
        iu_value_mapping_t values;
        std::vector<Sql::value_op_t> owned;
        for (auto & [iu, c] : columns) {
            owned.push_back(Sql::Value::load(_codeGen->CreateStructGEP(rowType, row, c), _state.types[c]));
            values[iu] = owned.back().get();
        }
        emit(values);
    });
}

} // end namespace Physical
} // end namespace Algebra
