#include "algebra/physical/Window.hpp"

#include "codegen/LLVMCompat.hpp"

#include <llvm/IR/DataLayout.h>

#include "algebra/physical/Aggregation.hpp"
#include "algebra/physical/Map.hpp"
#include "algebra/physical/Print.hpp"
#include "algebra/physical/Select.hpp"
#include "algebra/physical/TableScan.hpp"
#include "algebra/physical/TupleStream.hpp"
#include "foundations/Parallel.hpp"
#include "sql/SqlValues.hpp"

namespace Algebra {
namespace Physical {

using WindowEvaluation::Rows;

static void window_reset(void * rows)
{
    static_cast<Rows *>(rows)->reset();
}

static void * window_stage(void * rows, uint64_t worker)
{
    return static_cast<Rows *>(rows)->stage(worker);
}

static void window_insert(void * rows, uint64_t worker)
{
    static_cast<Rows *>(rows)->insert(worker);
}

static void window_run(void * rows)
{
    static_cast<Rows *>(rows)->run();
}

static uint64_t window_count(void * rows)
{
    return static_cast<Rows *>(rows)->count();
}

static void * window_row(void * rows, uint64_t i)
{
    return static_cast<Rows *>(rows)->order()[i];
}

/// The rows to the parent in morsels of the workers
static void window_emit(void * rows, void * function)
{
    Parallel::runMorsels(Parallel::threads, static_cast<Rows *>(rows)->count(), 1, reinterpret_cast<Parallel::morsel_function_t>(function));
}

Window::Window(const logical_operator_t & logicalOperator, std::unique_ptr<Operator> input, QueryContext & queryContext) :
        UnaryOperator(logicalOperator, std::move(input), queryContext)
{ }

Window::~Window()
{ }

const Logical::Window & Window::logical() const
{
    return static_cast<const Logical::Window &>(_logicalOperator);
}

bool Window::parallelInput() const
{
    Operator * op = child.get();
    while (dynamic_cast<Select *>(op) != nullptr || dynamic_cast<Map *>(op) != nullptr) op = op->getChildren().front();
    auto * scan = dynamic_cast<TableScan *>(op);
    auto * window = dynamic_cast<Window *>(op);
    return (scan != nullptr && scan->runsParallel()) || (window != nullptr && window->emitsParallel());
}

bool Window::emitsParallel() const
{
    if (Parallel::threads <= 1) return false;
    bool computed = false;
    for (Operator * op = _parent; op != nullptr; op = op->getParent()) {
        if (dynamic_cast<Select *>(op) != nullptr) continue;
        if (dynamic_cast<Map *>(op) != nullptr) { computed = true; continue; }
        if (dynamic_cast<Aggregation *>(op) != nullptr || dynamic_cast<Window *>(op) != nullptr) return true;
        return !computed && (dynamic_cast<TupleStream *>(op) != nullptr || dynamic_cast<Print *>(op) != nullptr);
    }
    return false;
}

void Window::emitRow(llvm::Value * index)
{
    llvm::Type * voidPtr = cg_voidptr_t::getType();
    llvm::Value * row = _codeGen.CreateCall(&window_row, llvm::FunctionType::get(voidPtr, {voidPtr, _codeGen->getInt64Ty()}, false), {rowsPointer(), index});
    iu_value_mapping_t values;
    std::vector<Sql::value_op_t> owned;
    for (size_t f = 0; f < _stored.size(); ++f) {
        owned.push_back(Sql::Value::load(_codeGen->CreateStructGEP(_rowType, row, static_cast<unsigned>(f)), _stored[f]->sqlType));
        values[_stored[f]] = owned.back().get();
    }
    emit(values);
}

llvm::Value * Window::rowsPointer()
{
    return cg_voidptr_t::fromRawPointer(_state->rows.get()).getValue();
}

void Window::produce()
{
    auto owned = std::make_unique<State>();
    _state = owned.get();
    _context.executionContext.acquireResource(std::move(owned));

    // the row: what the operator reads or passes on, then the results
    _stored.assign(getRequired().begin(), getRequired().end());
    _attributes = _stored.size();
    for (auto & call : logical()._calls) _stored.push_back(call.output);
    std::vector<llvm::Type *> members;
    for (iu_p_t iu : _stored) members.push_back(Sql::toLLVMTy(iu->sqlType));
    _rowType = llvm::StructType::get(_codeGen.getLLVMContext(), members);
    const llvm::DataLayout & dataLayout = _codeGen.getCurrentModuleGen().getDataLayout();
    const llvm::StructLayout * rowLayout = dataLayout.getStructLayout(_rowType);
    auto field = [&](iu_p_t iu) {
        const size_t index = static_cast<size_t>(std::find(_stored.begin(), _stored.end(), iu) - _stored.begin());
        WindowEvaluation::Field f;
        f.type = Sql::toNotNullableTy(iu->sqlType);
        f.offset = rowLayout->getElementOffset(static_cast<unsigned>(index));
        f.size = dataLayout.getTypeAllocSize(Sql::toLLVMTy(f.type));
        if (iu->sqlType.nullable) {
            // a struct of the value and its NULL indicator
            const llvm::StructLayout * nullable = dataLayout.getStructLayout(llvm::cast<llvm::StructType>(members[index]));
            f.nullOffset = f.offset + nullable->getElementOffset(1);
            f.offset += nullable->getElementOffset(0);
        }
        return f;
    };
    WindowEvaluation::Layout layout;
    layout.rowSize = dataLayout.getTypeAllocSize(_rowType);
    for (iu_p_t iu : logical()._partition) layout.partition.push_back(field(iu));
    for (auto & key : logical()._order) layout.order.push_back({field(key.input), key.descending, key.nullsFirst});
    for (auto & call : logical()._calls) {
        WindowEvaluation::Call c;
        c.function = call.function;
        for (iu_p_t a : call.arguments) c.arguments.push_back(field(a));
        c.output = field(call.output);
        c.mode = call.mode;
        c.start.kind = call.start.kind;
        c.end.kind = call.end.kind;
        if (call.start.offset != nullptr) c.start.offset = field(call.start.offset);
        if (call.end.offset != nullptr) c.end.offset = field(call.end.offset);
        layout.calls.push_back(std::move(c));
    }
    _parallel = parallelInput();
    _state->rows = std::make_unique<Rows>(std::move(layout), _parallel ? Parallel::threads : 1);

    // the tuples of the input into the rows (at each execution anew), the functions, then the rows to the parent
    llvm::Type * voidPtr = cg_voidptr_t::getType();
    llvm::Type * i64 = _codeGen->getInt64Ty();
    llvm::FunctionType * rowsTy = llvm::FunctionType::get(_codeGen->getVoidTy(), {voidPtr}, false);
    _codeGen.CreateCall(&window_reset, rowsTy, {rowsPointer()});
    child->produce();
    _codeGen.CreateCall(&window_run, rowsTy, {rowsPointer()});

    if (emitsParallel()) {
        // the loop over a morsel of the rows and the operators above it form their own function, as of a parallel scan
        auto & context = _codeGen.getLLVMContext();
        llvm::Function * morsel;
        {
            FunctionGen funcGen(_codeGen.getCurrentModuleGen(), "window_morsel_" + std::to_string(_logicalOperator.getUID()), llvm::TypeBuilder<void (size_t, size_t, size_t, size_t), false>::get(context));
            _context.parallelPipeline = true;
            llvm::Value * begin = funcGen.getArg(0);
            llvm::Value * end = funcGen.getArg(1);
            LoopGen loop(funcGen, _codeGen->CreateICmpULT(begin, end), {{"row", begin}});
            cg_size_t index(loop.getLoopVar(0));
            {
                LoopBodyGen body(loop);
                emitRow(index.getValue());
            }
            cg_size_t next = index + 1ul;
            loop.loopDone(_codeGen->CreateICmpULT(next.getValue(), end), {next});
            _context.parallelPipeline = false;
            morsel = funcGen.getFunction();
        }
        _codeGen.CreateCall(&window_emit, llvm::FunctionType::get(_codeGen->getVoidTy(), {voidPtr, voidPtr}, false),
                            {rowsPointer(), _codeGen->CreatePointerCast(morsel, voidPtr)});
        return;
    }
    auto & funcGen = _codeGen.getCurrentFunctionGen();
    llvm::Value * count = _codeGen.CreateCall(&window_count, llvm::FunctionType::get(i64, {voidPtr}, false), {rowsPointer()});
    LoopGen loop(funcGen, _codeGen->CreateICmpULT(_codeGen->getInt64(0), count), {{"row", cg_size_t(0ul)}});
    cg_size_t index(loop.getLoopVar(0));
    {
        LoopBodyGen body(loop);
        emitRow(index.getValue());
    }
    cg_size_t next = index + 1ul;
    loop.loopDone(_codeGen->CreateICmpULT(next.getValue(), count), {next});
}

void Window::consume(const iu_value_mapping_t & values, const Operator & src)
{
    // in the workers of a parallel scan, the hash groups of the worker
    const bool worker = _parallel && _context.parallelPipeline;
    llvm::Value * index = worker ? static_cast<llvm::Value *>(_codeGen.getCurrentFunctionGen().getArg(2)) : _codeGen->getInt64(0);
    llvm::Type * voidPtr = cg_voidptr_t::getType();
    llvm::Type * i64 = _codeGen->getInt64Ty();
    llvm::Value * row = _codeGen.CreateCall(&window_stage, llvm::FunctionType::get(voidPtr, {voidPtr, i64}, false), {rowsPointer(), index});
    for (size_t f = 0; f < _attributes; ++f) {
        const Sql::Value & value = *values.at(_stored[f]);
        Sql::value_op_t stored = (_stored[f]->sqlType.nullable && !Sql::isNullable(value)) ? Sql::NullableValue::create(value, cg_bool_t(false))
                                                                                            : value.clone();
        stored->store(_codeGen->CreateStructGEP(_rowType, row, static_cast<unsigned>(f)));
    }
    _codeGen.CreateCall(&window_insert, llvm::FunctionType::get(_codeGen->getVoidTy(), {voidPtr, i64}, false), {rowsPointer(), index});
}

} // end namespace Physical
} // end namespace Algebra
