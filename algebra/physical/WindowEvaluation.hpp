// The evaluation of window functions over buffered rows: hash groups, sorting, and the functions over partitions and frames

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "algebra/logical/operators.hpp"
#include "sql/SqlType.hpp"

namespace Algebra {
namespace Physical {
namespace WindowEvaluation {

using Function = Logical::Window::Function;
using Mode = Logical::Window::Mode;
using BoundKind = Logical::Window::BoundKind;

/// A value of a row: its bytes at offset, its NULL indicator at nullOffset (SIZE_MAX if it has none)
struct Field {
    size_t offset = 0;
    size_t nullOffset = SIZE_MAX;
    Sql::SqlType type; // without NULL
    size_t size = 0;   // of the value
};

struct Bound {
    BoundKind kind = BoundKind::CurrentRow;
    Field offset; // of Preceding and Following
};

struct Key {
    Field field;
    bool descending = false;
    bool nullsFirst = false;
};

struct Call {
    Function function;
    std::vector<Field> arguments;
    Field output;
    Mode mode = Mode::Range;
    Bound start, end;
};

/// The rows and what the functions read and write in them
struct Layout {
    size_t rowSize = 0;
    std::vector<Field> partition;
    std::vector<Key> order;
    std::vector<Call> calls;
};

/// The rows of an execution: workers insert them into hash groups of their own, run() computes the functions into them
class Rows {
public:
    Rows(Layout layout, size_t workers);

    ~Rows();

    /// Discards the rows (at the start of an execution)
    void reset();

    /// \returns The bytes of the next row of the worker, which insert() then adds
    uint8_t * stage(size_t worker);

    void insert(size_t worker);

    /// Combines the hash groups of the workers, sorts and evaluates them, in parallel
    void run();

    size_t count() const { return _order.size(); }

    /// \returns The rows in the order of their hash groups, each sorted
    uint8_t * const * order() const { return _order.data(); }

    struct Group;

private:
    struct Local {
        std::vector<uint8_t> stage;
        std::vector<std::vector<uint8_t>> groups;
    };

    Layout _layout;
    std::vector<Local> _locals;
    std::vector<Group *> _groups;
    std::vector<uint8_t *> _order;
};

} // end namespace WindowEvaluation
} // end namespace Physical
} // end namespace Algebra
