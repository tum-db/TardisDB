#pragma once

// Arrays of PostgreSQL: a tuple holds a 16-byte handle of an immutable representation, as for text

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "sql/SqlType.hpp"

namespace Native {
namespace Sql {
namespace Arrays {

using ::Sql::SqlType;

/// The address and length of a representation; the top bit of the length marks one in a query's arena (see persist)
struct Handle {
    const uint8_t * data = nullptr;
    uint64_t length = 0;

    static constexpr uint64_t temporary = uint64_t(1) << 63;

    size_t size() const { return length & ~temporary; }
    bool isTemporary() const { return (length & temporary) != 0; }
};
static_assert(sizeof(Handle) == 16, "an array takes 16 bytes in a tuple");

inline Handle loadHandle(const void * ptr) { Handle h; std::memcpy(&h, ptr, sizeof(h)); return h; }
inline void storeHandle(void * ptr, Handle h) { std::memcpy(ptr, &h, sizeof(h)); }

/// The memory of the arrays a query computes, released with the query's physical plan
class Arena {
public:
    Arena();
    ~Arena();
    Arena(const Arena &) = delete;
    Arena & operator =(const Arena &) = delete;

    uint8_t * allocate(size_t size);

private:
    std::mutex _mutex;
    std::vector<std::unique_ptr<uint8_t[]>> _chunks;
    const uint64_t _id; // the threads' current chunks belong to the arena of their id
};

/// The arena of a query, which lives until the query ends (the plan that generated the code goes before)
struct QueryArenas {
    std::unique_ptr<Arena> arena;
};

/// While a query is compiled, its expressions compute arrays in the arena of its QueryArenas
class ArenaScope {
public:
    explicit ArenaScope(QueryArenas & arenas);
    ~ArenaScope();
private:
    QueryArenas * _previous;
};

/// \returns The arena of the query being compiled (one for all, so that a thread keeps allocating from its chunk)
Arena * createQueryArena();

/// \returns The representation in the string pool (a handle that is not temporary)
Handle persist(Handle handle);

/// The size of an element; of a string element the 4-byte end of its characters
size_t elementSize(SqlType elementType);
bool isStringElement(SqlType elementType);

/// A representation: dims (u32) | widths, lower bounds (i32 each) | elements (zero bytes if NULL) | NULL bitmap | strings
class View {
public:
    View(Handle handle, SqlType elementType);

    uint32_t dims() const { return _dims; }
    uint32_t count() const { return _count; }
    int32_t width(uint32_t d) const { return load32(_base + 4 + 4 * d); }
    int32_t lower(uint32_t d) const { return load32(_base + 4 + 4 * (_dims + d)); }
    int32_t upper(uint32_t d) const { return lower(d) + width(d) - 1; }
    bool isNull(uint32_t i) const { return (_nulls[i >> 3] >> (i & 7)) & 1; }
    bool anyNull() const;
    /// the bytes of a fixed size element
    const uint8_t * fixed(uint32_t i) const { return _elements + i * _elementSize; }
    /// the characters of a string element
    std::string_view string(uint32_t i) const;

    const SqlType elementType;

private:
    static int32_t load32(const uint8_t * p) { int32_t v; std::memcpy(&v, p, 4); return v; }

    const uint8_t * _base;
    uint32_t _dims = 0, _count = 0;
    size_t _elementSize;
    const uint8_t * _elements, * _nulls, * _strings;
};

/// Builds a representation from its shape and its elements in storage order (the last subscript varies fastest)
class Builder {
public:
    explicit Builder(SqlType elementType);

    std::vector<int32_t> widths, lowers; // no dimensions: the empty array (no elements)

    void addNull();
    void addFixed(const void * bytes);
    void addString(std::string_view characters);
    void add(const View & view, uint32_t i);
    uint32_t count() const { return static_cast<uint32_t>(_nulls.size()); }

    /// \returns The representation in the arena or, without one, in the string pool
    Handle finish(Arena * arena) const;

private:
    SqlType _elementType;
    size_t _elementSize;
    bool _strings;
    std::string _elements, _characters;
    std::vector<bool> _nulls;
};

/// The text of PostgreSQL ('{1,2}', '[0:1]={1,2}', '{{"a b",NULL}}') to an array of the given type
Handle parse(const std::string & text, SqlType arrayType, Arena * arena);
std::string format(Handle handle, SqlType arrayType);

/// The text of an element as array_to_string and the text of an array show it (unquoted)
std::string formatElement(const View & view, uint32_t i);

/// Equal shape and equal elements (NULL elements equal each other), as PostgreSQL's array_eq
bool equals(Handle a, Handle b, SqlType arrayType);
/// The order of PostgreSQL's btree operators of arrays
int compare(Handle a, Handle b, SqlType arrayType);
uint64_t hash(Handle a, SqlType arrayType);

/// The functions and operators of arrays, which the generated code calls by number
enum class Function : uint32_t {
    Construct,      // ARRAY[e, ...] of elements, or of arrays (one dimension more)
    Subscript,      // a[i][j]...
    Slice,          // a[l:u]...: two bounds per dimension; an omitted bound is INT32_MIN or INT32_MAX
    Append,         // array_append(a, e), a || e
    Prepend,        // array_prepend(e, a), e || a
    Cat,            // array_cat(a, b), a || b
    Length,         // array_length(a, d)
    Lower,          // array_lower(a, d)
    Upper,          // array_upper(a, d)
    Ndims,          // array_ndims(a)
    Dims,           // array_dims(a)
    Cardinality,    // cardinality(a)
    Position,       // array_position(a, e [, start])
    Positions,      // array_positions(a, e)
    Remove,         // array_remove(a, e)
    Replace,        // array_replace(a, old, new)
    Fill,           // array_fill(e, dims [, lower bounds])
    ToString,       // array_to_string(a, delimiter [, null string])
    FromString,     // string_to_array(s, delimiter [, null string])
    Trim,           // trim_array(a, n)
    Contains,       // a @> b
    ContainedBy,    // a <@ b
    Overlaps,       // a && b
    AnyCompare,     // e op ANY (a): third argument the comparison (ComparisonMode or 5 for <>)
    AllCompare,     // e op ALL (a)
    Equals,         // a = b of arrays (with NULL as false and NULL inputs handled by the caller)
    // linear algebra (without BLAS): arrays of numbers, computed in double precision
    Elementwise,    // x op y, an array or a number each: third argument + - * / ^ (ElementwiseOp)
    MatrixProduct,  // a ** b: sums over the last dimension of a and the first of b (a matrix product)
    Kronecker,      // kroneckerproduct(a, b) of two matrices
    Transpose,      // transpose(a): the first two dimensions swapped; a vector becomes a column
    Map,            // f(x) of each element, or of a number: second argument @/abs, sig, relu, relup, tanh (MapOp)
    Softmax,        // softmax(a) over all elements
    SoftmaxCce,     // softmaxcce(a, labels): the categorical cross-entropy of softmax(a) and the labels
    SoftmaxCceP,    // softmaxccep(a, labels): its gradient, softmax(a) - labels
    HighestPosition,// highestposition(a): the offset (from 0) of the first greatest element
};

/// The numbers of an array (of integer, bigint, numeric or double precision elements) as doubles, with its shape
struct NumberArray {
    std::vector<int32_t> widths, lowers; // none: the empty array
    std::vector<double> values;          // 0 for a NULL element
    std::vector<uint8_t> nulls;          // per element, none without NULL elements
    size_t count() const { return values.size(); }
    bool anyNull() const { return !nulls.empty(); }
    bool isNull(size_t i) const { return !nulls.empty() && nulls[i] != 0; }
};
NumberArray toNumbers(Handle handle, SqlType elementType);
/// Adds the numbers of an array of the same shape to \p sum (a NULL element makes its sum NULL)
void addNumbers(NumberArray & sum, Handle handle, SqlType elementType);
void addNumbers(NumberArray & sum, const NumberArray & add);
/// \returns The array of double precision of the numbers, in the arena or, without one, in the string pool
Handle fromNumbers(const NumberArray & numbers, Arena * arena);

enum class ElementwiseOp : int32_t { Add, Sub, Mul, Div, Pow };
enum class MapOp : int32_t { Abs, Sig, Relu, ReluP, Tanh };

/// Writes the result of a function of stored values (with NULL indicators and types as 64 bits) and \returns whether it is NULL
extern "C" bool tardisArrayCall(uint32_t function, Arena * arena, uint64_t resultType, void * result,
                                uint32_t argc, void * const * args, const bool * nulls, const uint64_t * types);

/// For the generated code: the operators of comparisons and hash joins
extern "C" bool tardisArrayEquals(const void * a, const void * b, uint64_t arrayType);
extern "C" int32_t tardisArrayCompare(const void * a, const void * b, uint64_t arrayType);
extern "C" uint64_t tardisArrayHash(const void * a, uint64_t arrayType);

inline uint64_t typeBits(SqlType type) { uint64_t bits; std::memcpy(&bits, &type, sizeof(bits)); return bits; }
inline SqlType typeOfBits(uint64_t bits) { SqlType type; std::memcpy(&type, &bits, sizeof(bits)); return type; }

} // end namespace Arrays
} // end namespace Sql
} // end namespace Native
