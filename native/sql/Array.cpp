#include "native/sql/Array.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <limits>
#include <functional>
#include <stdexcept>

#include "foundations/LegacyTypes.hpp"
#include "foundations/StringPool.hpp"
#include "native/sql/SqlValues.hpp"
#include "utils/hashing.hpp"

namespace Native {
namespace Sql {
namespace Arrays {

using TypeID = SqlType::TypeID;
using namespace HashUtils;

static std::runtime_error arrayError(const std::string & message) { return std::runtime_error(message); }

//-----------------------------------------------------------------------------
// memory

static std::atomic<uint64_t> arenaIds{1};

namespace {
/// the chunk a thread allocates from, of the arena with the id
struct ArenaCache {
    uint64_t id = 0;
    uint8_t * next = nullptr;
    size_t left = 0;
};
thread_local ArenaCache arenaCache;
}

Arena::Arena() : _id(arenaIds.fetch_add(1)) { }

Arena::~Arena() = default;

uint8_t * Arena::allocate(size_t size)
{
    size = (size + 7) & ~size_t(7);
    if (arenaCache.id == _id && arenaCache.left >= size) {
        uint8_t * p = arenaCache.next;
        arenaCache.next += size;
        arenaCache.left -= size;
        return p;
    }
    constexpr size_t chunkSize = 64 * 1024;
    std::lock_guard<std::mutex> lock(_mutex);
    if (size > chunkSize / 4) {
        _chunks.emplace_back(new uint8_t[size]);
        return _chunks.back().get();
    }
    _chunks.emplace_back(new uint8_t[chunkSize]);
    uint8_t * chunk = _chunks.back().get();
    arenaCache = {_id, chunk + size, chunkSize - size};
    return chunk;
}

static thread_local QueryArenas * currentArenas = nullptr;

ArenaScope::ArenaScope(QueryArenas & arenas) : _previous(currentArenas)
{
    currentArenas = &arenas;
}

ArenaScope::~ArenaScope()
{
    currentArenas = _previous;
}

Arena * createQueryArena()
{
    if (currentArenas == nullptr) throw std::logic_error("arrays are computed outside of a query");
    if (!currentArenas->arena) currentArenas->arena = std::make_unique<Arena>();
    return currentArenas->arena.get();
}

Handle persist(Handle handle)
{
    if (!handle.isTemporary()) return handle;
    size_t size = handle.size();
    std::unique_ptr<uint8_t[]> data(new uint8_t[std::max<size_t>(size, 1)]);
    std::memcpy(data.get(), handle.data, size);
    auto & stored = StringPool::instance().put(sql_string_t(size, std::move(data)));
    return Handle{stored.second.get(), size};
}

//-----------------------------------------------------------------------------
// representation

bool isStringElement(SqlType elementType)
{
    switch (elementType.typeID) {
        case TypeID::CharID: case TypeID::VarcharID: case TypeID::TextID: return true;
        default: return false;
    }
}

size_t elementSize(SqlType elementType)
{
    switch (elementType.typeID) {
        case TypeID::BoolID: return 1;
        case TypeID::IntegerID: case TypeID::DateID: return 4;
        case TypeID::LongIntegerID: case TypeID::NumericID: case TypeID::TimestampID: case TypeID::DoubleID: return 8;
        case TypeID::CharID: case TypeID::VarcharID: case TypeID::TextID: return 4;
        default: throw arrayError("arrays of type " + ::Sql::getName(elementType) + " are not supported");
    }
}

static size_t nullBytes(uint32_t count) { return (count + 7) / 8; }

/// the representation of the empty array: no dimensions
static const uint8_t emptyRepresentation[4] = {0, 0, 0, 0};

View::View(Handle handle, SqlType elementType) :
        elementType(elementType),
        _base(handle.size() >= 4 ? handle.data : emptyRepresentation),
        _elementSize(elementSize(elementType))
{
    std::memcpy(&_dims, _base, 4);
    _count = _dims ? 1 : 0;
    for (uint32_t d = 0; d < _dims; ++d) _count *= static_cast<uint32_t>(width(d));
    _elements = _base + 4 + 8 * _dims;
    _nulls = _elements + _count * _elementSize;
    _strings = _nulls + nullBytes(_count);
}

bool View::anyNull() const
{
    for (size_t b = 0; b < nullBytes(_count); ++b) if (_nulls[b] != 0) return true;
    return false;
}

std::string_view View::string(uint32_t i) const
{
    uint32_t begin = 0, end;
    if (i > 0) std::memcpy(&begin, _elements + 4 * (i - 1), 4);
    std::memcpy(&end, _elements + 4 * i, 4);
    return std::string_view(reinterpret_cast<const char *>(_strings) + begin, end - begin);
}

Builder::Builder(SqlType elementType) :
        _elementType(elementType),
        _elementSize(elementSize(elementType)),
        _strings(isStringElement(elementType))
{ }

void Builder::addNull()
{
    _nulls.push_back(true);
    if (_strings) {
        uint32_t end = static_cast<uint32_t>(_characters.size());
        _elements.append(reinterpret_cast<const char *>(&end), 4);
    } else {
        _elements.append(_elementSize, '\0');
    }
}

void Builder::addFixed(const void * bytes)
{
    _nulls.push_back(false);
    _elements.append(static_cast<const char *>(bytes), _elementSize);
}

void Builder::addString(std::string_view characters)
{
    _nulls.push_back(false);
    _characters.append(characters.data(), characters.size());
    if (_characters.size() > UINT32_MAX) throw arrayError("array too large");
    uint32_t end = static_cast<uint32_t>(_characters.size());
    _elements.append(reinterpret_cast<const char *>(&end), 4);
}

void Builder::add(const View & view, uint32_t i)
{
    if (view.isNull(i)) addNull();
    else if (_strings) addString(view.string(i));
    else addFixed(view.fixed(i));
}

Handle Builder::finish(Arena * arena) const
{
    uint32_t dims = static_cast<uint32_t>(widths.size());
    uint64_t expected = dims ? 1 : 0;
    for (int32_t w : widths) {
        if (w < 0) throw arrayError("array dimensions must not be negative");
        expected *= static_cast<uint64_t>(w);
    }
    if (expected == 0) dims = 0; // without elements, an array is the empty array
    if (expected != count()) throw arrayError("array size does not match its dimensions");
    if (lowers.size() != widths.size()) throw arrayError("array lower bounds do not match its dimensions");
    if (expected > INT32_MAX) throw arrayError("array size exceeds the maximum allowed");
    for (uint32_t d = 0; d < dims; ++d) {
        if (static_cast<int64_t>(lowers[d]) + widths[d] - 1 > INT32_MAX) throw arrayError("array upper bound is too large");
    }

    size_t size = 4 + 8 * dims + _elements.size() + nullBytes(count()) + _characters.size();
    std::unique_ptr<uint8_t[]> pooled;
    uint8_t * out;
    if (arena) {
        out = arena->allocate(size);
    } else {
        pooled.reset(new uint8_t[size]);
        out = pooled.get();
    }
    uint8_t * w = out;
    std::memcpy(w, &dims, 4); w += 4;
    if (dims) {
        std::memcpy(w, widths.data(), 4 * dims); w += 4 * dims;
        std::memcpy(w, lowers.data(), 4 * dims); w += 4 * dims;
    }
    std::memcpy(w, _elements.data(), _elements.size()); w += _elements.size();
    std::memset(w, 0, nullBytes(count()));
    for (uint32_t i = 0; i < count(); ++i) if (_nulls[i]) w[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
    w += nullBytes(count());
    std::memcpy(w, _characters.data(), _characters.size());

    if (arena) return Handle{out, size | Handle::temporary};
    auto & stored = StringPool::instance().put(sql_string_t(size, std::move(pooled)));
    return Handle{stored.second.get(), size};
}

//-----------------------------------------------------------------------------
// elements

static int64_t load64(const uint8_t * p) { int64_t v; std::memcpy(&v, p, 8); return v; }
static int32_t load32(const uint8_t * p) { int32_t v; std::memcpy(&v, p, 4); return v; }
static double loadDouble(const uint8_t * p) { double v; std::memcpy(&v, p, 8); return v; }

/// The order of doubles in PostgreSQL: NaN equals NaN and is greater than all other values; -0 equals 0
static int compareDoubles(double a, double b)
{
    bool aNaN = std::isnan(a), bNaN = std::isnan(b);
    if (aNaN || bNaN) return aNaN == bNaN ? 0 : (aNaN ? 1 : -1);
    return a < b ? -1 : (a > b ? 1 : 0);
}

/// The order of two non-NULL elements of the same type
static int compareElements(const View & a, uint32_t i, const View & b, uint32_t j)
{
    switch (a.elementType.typeID) {
        case TypeID::BoolID: return static_cast<int>(*a.fixed(i) != 0) - static_cast<int>(*b.fixed(j) != 0);
        case TypeID::IntegerID: { int32_t x = load32(a.fixed(i)), y = load32(b.fixed(j)); return x < y ? -1 : (x > y); }
        case TypeID::DateID: { uint32_t x = static_cast<uint32_t>(load32(a.fixed(i))), y = static_cast<uint32_t>(load32(b.fixed(j))); return x < y ? -1 : (x > y); }
        case TypeID::LongIntegerID: case TypeID::NumericID: { int64_t x = load64(a.fixed(i)), y = load64(b.fixed(j)); return x < y ? -1 : (x > y); }
        case TypeID::TimestampID: { uint64_t x = static_cast<uint64_t>(load64(a.fixed(i))), y = static_cast<uint64_t>(load64(b.fixed(j))); return x < y ? -1 : (x > y); }
        case TypeID::DoubleID: return compareDoubles(loadDouble(a.fixed(i)), loadDouble(b.fixed(j)));
        default: {
            std::string_view x = a.string(i), y = b.string(j);
            int c = x.compare(y);
            return c < 0 ? -1 : (c > 0);
        }
    }
}

static uint64_t hashElement(const View & a, uint32_t i)
{
    if (a.isNull(i)) return 0x9e3779b97f4a7c15ull;
    switch (a.elementType.typeID) {
        case TypeID::BoolID: return hashInteger<uint64_t>(*a.fixed(i) != 0);
        case TypeID::IntegerID: case TypeID::DateID: return hashInteger<uint64_t>(static_cast<uint32_t>(load32(a.fixed(i))));
        case TypeID::LongIntegerID: case TypeID::NumericID: case TypeID::TimestampID: return hashInteger<uint64_t>(static_cast<uint64_t>(load64(a.fixed(i))));
        case TypeID::DoubleID: {
            double d = loadDouble(a.fixed(i));
            if (std::isnan(d)) return 0x7ff8000000000000ull;
            d += 0.0; // -0 hashes as 0
            uint64_t bits;
            std::memcpy(&bits, &d, 8);
            return hashInteger<uint64_t>(bits);
        }
        default: {
            std::string_view s = a.string(i);
            return hashByteArray(reinterpret_cast<const uint8_t *>(s.data()), s.size());
        }
    }
}

/// whether elements are not distinct (both NULL or equal)
static bool notDistinct(const View & a, uint32_t i, const View & b, uint32_t j)
{
    if (a.isNull(i) || b.isNull(j)) return a.isNull(i) && b.isNull(j);
    return compareElements(a, i, b, j) == 0;
}

/// NULL in any case
static bool isNullWord(const std::string & s)
{
    return s.size() == 4 && (s[0] == 'n' || s[0] == 'N') && (s[1] == 'u' || s[1] == 'U') && (s[2] == 'l' || s[2] == 'L') && (s[3] == 'l' || s[3] == 'L');
}

static bool needsQuotes(const std::string & s)
{
    if (s.empty() || isNullWord(s)) return true;
    for (char c : s) {
        if (c == '{' || c == '}' || c == ',' || c == '"' || c == '\\' || std::isspace(static_cast<unsigned char>(c))) return true;
    }
    return false;
}

std::string formatElement(const View & view, uint32_t i)
{
    const uint8_t * p = view.fixed(i);
    switch (view.elementType.typeID) {
        case TypeID::BoolID: return *p ? "t" : "f";
        case TypeID::IntegerID: return std::to_string(load32(p));
        case TypeID::LongIntegerID: return std::to_string(load64(p));
        case TypeID::NumericID: return formatNumeric(load64(p), view.elementType.numericLayout.precision);
        case TypeID::DoubleID: return formatDouble(loadDouble(p));
        case TypeID::DateID: return formatDate(static_cast<uint32_t>(load32(p)));
        case TypeID::TimestampID: return formatTimestamp(static_cast<uint64_t>(load64(p)));
        default: return std::string(view.string(i));
    }
}

/// The element of the text in the element type of the builder
static void addParsed(Builder & builder, SqlType elementType, const std::string & text)
{
    switch (elementType.typeID) {
        case TypeID::CharID:
        case TypeID::VarcharID: {
            if (text.size() > elementType.length) {
                throw arrayError("value too long for type " + std::string(elementType.typeID == TypeID::CharID ? "character" : "character varying") +
                                 "(" + std::to_string(elementType.length) + ")");
            }
            if (elementType.typeID == TypeID::CharID && text.size() < elementType.length) {
                builder.addString(text + std::string(elementType.length - text.size(), ' ')); // char(n) is padded
            } else {
                builder.addString(text);
            }
            return;
        }
        case TypeID::TextID:
            builder.addString(text);
            return;
        default: {
            uint8_t bytes[16] = {};
            Value::castString(text, ::Sql::toNotNullableTy(elementType))->store(bytes);
            builder.addFixed(bytes);
            return;
        }
    }
}

//-----------------------------------------------------------------------------
// text

namespace {
class Parser {
public:
    Parser(const std::string & text, SqlType elementType) : _text(text), _elementType(elementType), _builder(elementType) { }

    Handle run(Arena * arena)
    {
        skipSpaces();
        std::vector<int32_t> declaredLowers, declaredWidths;
        if (peek() == '[') {
            // [l:u][l:u]=
            while (peek() == '[') {
                ++_pos;
                int64_t first = number(), second;
                skipSpaces();
                if (peek() == ':') {
                    ++_pos;
                    second = number();
                } else {
                    second = first;
                    first = 1;
                }
                skipSpaces();
                expect(']');
                if (second < first - 1) fail("upper bound cannot be less than lower bound");
                declaredLowers.push_back(static_cast<int32_t>(first));
                declaredWidths.push_back(static_cast<int32_t>(second - first + 1));
                skipSpaces();
            }
            expect('=');
            skipSpaces();
        }
        if (peek() != '{') fail("array value must start with \"{\" or dimension information");
        level(0);
        skipSpaces();
        if (_pos != _text.size()) fail("junk after closing right brace");

        if (_builder.count() == 0) {
            _widths.clear();
        }
        if (!declaredWidths.empty()) {
            if (declaredWidths != _widths) fail("specified array dimensions do not match array contents");
            _builder.lowers = declaredLowers;
        } else {
            _builder.lowers.assign(_widths.size(), 1);
        }
        _builder.widths = _widths;
        return _builder.finish(arena);
    }

private:
    [[noreturn]] void fail(const std::string & detail)
    {
        throw arrayError("malformed array literal: \"" + _text + "\": " + detail);
    }

    char peek() const { return _pos < _text.size() ? _text[_pos] : '\0'; }
    void skipSpaces() { while (_pos < _text.size() && std::isspace(static_cast<unsigned char>(_text[_pos]))) ++_pos; }
    void expect(char c) { if (peek() != c) fail(std::string("expected \"") + c + "\""); ++_pos; }

    int64_t number()
    {
        skipSpaces();
        size_t begin = _pos;
        if (peek() == '-' || peek() == '+') ++_pos;
        while (std::isdigit(static_cast<unsigned char>(peek()))) ++_pos;
        if (begin == _pos || (_pos == begin + 1 && !std::isdigit(static_cast<unsigned char>(_text[begin])))) fail("missing array dimension");
        int64_t value = std::stoll(_text.substr(begin, _pos - begin));
        if (value < INT32_MIN || value > INT32_MAX) fail("array bound is out of range");
        return value;
    }

    /// { ... } at the depth (0: outermost)
    void level(size_t depth)
    {
        expect('{');
        if (depth >= 6) fail("number of array dimensions exceeds the maximum allowed (6)");
        if (_widths.size() <= depth) _widths.resize(depth + 1, -1);
        skipSpaces();
        int32_t items = 0;
        if (peek() == '}') {
            ++_pos;
            if (depth != 0) fail("unexpected \"}\" character");
            _widths.resize(1);
            _widths[0] = 0;
            _leafDepth = 0;
            return;
        }
        for (;;) {
            skipSpaces();
            if (peek() == '{') {
                if (_leafDepth >= 0 && _leafDepth <= static_cast<int>(depth)) fail("unexpected \"{\" character");
                level(depth + 1);
            } else {
                if (_leafDepth < 0) _leafDepth = static_cast<int>(depth);
                if (_leafDepth != static_cast<int>(depth)) fail("multidimensional arrays must have sub-arrays with matching dimensions");
                element();
            }
            ++items;
            skipSpaces();
            if (peek() == ',') { ++_pos; continue; }
            if (peek() == '}') { ++_pos; break; }
            fail("unexpected end of input");
        }
        if (_widths[depth] < 0) _widths[depth] = items;
        else if (_widths[depth] != items) fail("multidimensional arrays must have sub-arrays with matching dimensions");
        if (depth == 0) _widths.resize(static_cast<size_t>(_leafDepth) + 1);
    }

    void element()
    {
        std::string value;
        bool quoted = false;
        if (peek() == '"') {
            quoted = true;
            ++_pos;
            for (;;) {
                if (_pos >= _text.size()) fail("unexpected end of input");
                char c = _text[_pos++];
                if (c == '"') break;
                if (c == '\\') {
                    if (_pos >= _text.size()) fail("unexpected end of input");
                    c = _text[_pos++];
                }
                value += c;
            }
        } else {
            size_t lastNonSpace = 0;
            for (;;) {
                if (_pos >= _text.size()) fail("unexpected end of input");
                char c = _text[_pos];
                if (c == ',' || c == '}') break;
                if (c == '{' || c == '"') fail(std::string("unexpected \"") + c + "\" character");
                ++_pos;
                if (c == '\\') {
                    if (_pos >= _text.size()) fail("unexpected end of input");
                    value += _text[_pos++];
                    lastNonSpace = value.size();
                    continue;
                }
                value += c;
                if (!std::isspace(static_cast<unsigned char>(c))) lastNonSpace = value.size();
            }
            value.resize(lastNonSpace); // trailing spaces
            if (value.empty()) fail("unexpected \"" + std::string(1, peek()) + "\" character");
        }
        if (!quoted && isNullWord(value)) {
            _builder.addNull();
            return;
        }
        try {
            addParsed(_builder, _elementType, value);
        } catch (const std::exception & e) {
            throw arrayError(std::string(e.what()) + " (in array literal \"" + _text + "\")");
        } catch (const char * e) {
            throw arrayError(std::string(e) + " (in array literal \"" + _text + "\")");
        }
    }

    const std::string & _text;
    SqlType _elementType;
    Builder _builder;
    size_t _pos = 0;
    std::vector<int32_t> _widths;
    int _leafDepth = -1; // the depth of the elements
};
}

Handle parse(const std::string & text, SqlType arrayType, Arena * arena)
{
    return Parser(text, ::Sql::getElementTy(arrayType)).run(arena);
}

std::string format(Handle handle, SqlType arrayType)
{
    View view(handle, ::Sql::getElementTy(arrayType));
    if (view.count() == 0) return "{}";
    std::string out;
    bool decorate = false;
    for (uint32_t d = 0; d < view.dims(); ++d) decorate = decorate || view.lower(d) != 1;
    if (decorate) {
        for (uint32_t d = 0; d < view.dims(); ++d) {
            out += "[" + std::to_string(view.lower(d)) + ":" + std::to_string(view.upper(d)) + "]";
        }
        out += "=";
    }
    std::vector<int32_t> index(view.dims(), 0);
    for (uint32_t i = 0; i < view.count(); ++i) {
        // open the dimensions that start here
        uint32_t opening = 0;
        for (uint32_t d = view.dims(); d > 0 && index[d - 1] == 0; --d) ++opening;
        if (i > 0) out += ',';
        out.append(opening, '{');
        if (view.isNull(i)) {
            out += "NULL";
        } else {
            std::string s = formatElement(view, i);
            if (needsQuotes(s)) {
                out += '"';
                for (char c : s) {
                    if (c == '"' || c == '\\') out += '\\';
                    out += c;
                }
                out += '"';
            } else {
                out += s;
            }
        }
        // close the dimensions that end here, and advance the index
        uint32_t d = view.dims();
        while (d > 0) {
            if (++index[d - 1] < view.width(d - 1)) break;
            index[d - 1] = 0;
            out += '}';
            --d;
        }
    }
    return out;
}

//-----------------------------------------------------------------------------
// comparisons

bool equals(Handle a, Handle b, SqlType arrayType)
{
    SqlType elementType = ::Sql::getElementTy(arrayType);
    View x(a, elementType), y(b, elementType);
    if (x.dims() != y.dims()) return false;
    for (uint32_t d = 0; d < x.dims(); ++d) {
        if (x.width(d) != y.width(d) || x.lower(d) != y.lower(d)) return false;
    }
    for (uint32_t i = 0; i < x.count(); ++i) {
        if (!notDistinct(x, i, y, i)) return false;
    }
    return true;
}

int compare(Handle a, Handle b, SqlType arrayType)
{
    SqlType elementType = ::Sql::getElementTy(arrayType);
    View x(a, elementType), y(b, elementType);
    uint32_t n = std::min(x.count(), y.count());
    for (uint32_t i = 0; i < n; ++i) {
        bool xNull = x.isNull(i), yNull = y.isNull(i);
        if (xNull || yNull) {
            if (xNull != yNull) return xNull ? 1 : -1; // NULLs last
            continue;
        }
        int c = compareElements(x, i, y, i);
        if (c != 0) return c;
    }
    // equal elements: the longer array is greater, then the shape decides
    if (x.count() != y.count()) return x.count() < y.count() ? -1 : 1;
    if (x.dims() != y.dims()) return x.dims() < y.dims() ? -1 : 1;
    for (uint32_t d = 0; d < x.dims(); ++d) {
        if (x.width(d) != y.width(d)) return x.width(d) < y.width(d) ? -1 : 1;
        if (x.lower(d) != y.lower(d)) return x.lower(d) < y.lower(d) ? -1 : 1;
    }
    return 0;
}

uint64_t hash(Handle a, SqlType arrayType)
{
    View x(a, ::Sql::getElementTy(arrayType));
    uint64_t h = 0xcbf29ce484222325ull ^ x.dims();
    for (uint32_t d = 0; d < x.dims(); ++d) h = h * 31 + static_cast<uint32_t>(x.width(d)) * 7 + static_cast<uint32_t>(x.lower(d));
    for (uint32_t i = 0; i < x.count(); ++i) h = h * 0x100000001b3ull ^ hashElement(x, i);
    return h;
}

//-----------------------------------------------------------------------------
// functions

namespace {

/// The arguments of a call from the generated code
struct Args {
    uint32_t argc;
    void * const * args;
    const bool * nulls;
    const uint64_t * types;

    bool null(uint32_t i) const { return nulls[i]; }
    SqlType type(uint32_t i) const { return typeOfBits(types[i]); }
    Handle array(uint32_t i) const { return loadHandle(args[i]); }
    int32_t integer(uint32_t i) const { return load32(static_cast<const uint8_t *>(args[i])); }
    std::string text(uint32_t i) const { return toString(*Value::load(args[i], ::Sql::toNotNullableTy(type(i)))); }
};

/// The bytes of a fixed size element, or the characters of a string element, of a value as the generated code stores it
struct Element {
    bool null = true;
    uint8_t fixed[16] = {};
    std::string characters;
};

Element elementOf(const Args & a, uint32_t i, SqlType elementType)
{
    Element e;
    if (a.null(i)) return e;
    e.null = false;
    if (isStringElement(elementType)) {
        e.characters = a.text(i);
        if (elementType.typeID == TypeID::CharID || elementType.typeID == TypeID::VarcharID) {
            if (e.characters.size() > elementType.length) {
                throw arrayError("value too long for type " + std::string(elementType.typeID == TypeID::CharID ? "character" : "character varying") +
                                 "(" + std::to_string(elementType.length) + ")");
            }
            if (elementType.typeID == TypeID::CharID) e.characters.resize(elementType.length, ' ');
        }
    } else {
        std::memcpy(e.fixed, a.args[i], elementSize(elementType));
    }
    return e;
}

void addElement(Builder & builder, const Element & e, SqlType elementType)
{
    if (e.null) builder.addNull();
    else if (isStringElement(elementType)) builder.addString(e.characters);
    else builder.addFixed(e.fixed);
}

/// An array of one element, to compare an element with the elements of an array
Handle single(const Element & e, SqlType elementType, Arena * arena)
{
    Builder builder(elementType);
    builder.widths = {1};
    builder.lowers = {1};
    addElement(builder, e, elementType);
    return builder.finish(arena);
}

/// Writes element i of the view as the generated code loads a value of the element type
void writeElement(const View & view, uint32_t i, Handle source, void * result)
{
    SqlType type = view.elementType;
    switch (type.typeID) {
        case TypeID::TextID: {
            std::string_view s = view.string(i);
            if (s.size() <= 15) {
                uint8_t bytes[16] = {};
                bytes[0] = static_cast<uint8_t>(s.size());
                std::memcpy(bytes + 1, s.data(), s.size());
                std::memcpy(result, bytes, 16);
            } else if (!source.isTemporary()) {
                // the characters of a stored array live as long as the database
                uintptr_t words[2] = {reinterpret_cast<uintptr_t>(s.data()) ^ (uintptr_t(1) << 63), reinterpret_cast<uintptr_t>(s.data() + s.size())};
                std::memcpy(result, words, 16);
            } else {
                Text::castString(std::string(s))->store(result);
            }
            return;
        }
        case TypeID::CharID:
        case TypeID::VarcharID: {
            std::string s(view.string(i));
            Value::castString(s, ::Sql::toNotNullableTy(type))->store(result);
            return;
        }
        default:
            std::memcpy(result, view.fixed(i), elementSize(type));
            return;
    }
}

void writeInteger(void * result, int32_t value) { std::memcpy(result, &value, 4); }
void writeBool(void * result, bool value) { *static_cast<uint8_t *>(result) = value ? 1 : 0; }
void writeText(void * result, const std::string & s) { Text::castString(s)->store(result); }

int64_t subscriptIndex(const View & view, const std::vector<int32_t> & index)
{
    if (index.size() != view.dims() || view.count() == 0) return -1;
    int64_t position = 0;
    for (uint32_t d = 0; d < view.dims(); ++d) {
        if (index[d] < view.lower(d) || index[d] > view.upper(d)) return -1;
        position = position * view.width(d) + (index[d] - view.lower(d));
    }
    return position;
}

void requireOneDimension(const View & view, const char * what)
{
    if (view.dims() > 1) throw arrayError(std::string(what) + " in multidimensional arrays is not supported");
}

Handle concatenate(Handle a, Handle b, SqlType elementType, Arena * arena)
{
    View x(a, elementType), y(b, elementType);
    if (x.count() == 0) return b;
    if (y.count() == 0) return a;
    Builder builder(elementType);
    if (x.dims() == y.dims()) {
        for (uint32_t d = 1; d < x.dims(); ++d) {
            if (x.width(d) != y.width(d)) throw arrayError("cannot concatenate incompatible arrays: arrays with differing dimensions");
        }
        for (uint32_t d = 0; d < x.dims(); ++d) { builder.widths.push_back(x.width(d)); builder.lowers.push_back(x.lower(d)); }
        builder.widths[0] += y.width(0);
    } else if (x.dims() + 1 == y.dims()) {
        // the first argument becomes the first element of the outer dimension
        for (uint32_t d = 0; d < x.dims(); ++d) {
            if (x.width(d) != y.width(d + 1)) throw arrayError("cannot concatenate incompatible arrays: arrays with differing element dimensions");
        }
        for (uint32_t d = 0; d < y.dims(); ++d) { builder.widths.push_back(y.width(d)); builder.lowers.push_back(y.lower(d)); }
        builder.widths[0] += 1;
    } else if (x.dims() == y.dims() + 1) {
        for (uint32_t d = 0; d < y.dims(); ++d) {
            if (y.width(d) != x.width(d + 1)) throw arrayError("cannot concatenate incompatible arrays: arrays with differing element dimensions");
        }
        for (uint32_t d = 0; d < x.dims(); ++d) { builder.widths.push_back(x.width(d)); builder.lowers.push_back(x.lower(d)); }
        builder.widths[0] += 1;
    } else {
        throw arrayError("cannot concatenate incompatible arrays: arrays of " + std::to_string(x.dims()) + " and " +
                         std::to_string(y.dims()) + " dimensions are not compatible for concatenation");
    }
    for (uint32_t i = 0; i < x.count(); ++i) builder.add(x, i);
    for (uint32_t i = 0; i < y.count(); ++i) builder.add(y, i);
    return builder.finish(arena);
}

/// e op element, three-valued: 1 true, 0 false, -1 NULL
int compareWith(const View & e, const View & array, uint32_t i, uint32_t op)
{
    if (e.isNull(0) || array.isNull(i)) return -1;
    int c = compareElements(e, 0, array, i);
    switch (op) {
        case 0: return c < 0;   // less
        case 1: return c <= 0;  // leq
        case 2: return c == 0;  // eq
        case 3: return c >= 0;  // geq
        case 4: return c > 0;   // gtr
        default: return c != 0; // <>
    }
}

std::vector<int32_t> integerList(Handle handle, const char * what)
{
    View view(handle, ::Sql::getIntegerTy());
    if (view.dims() > 1) throw arrayError(std::string("wrong number of array subscripts: ") + what + " must be one-dimensional");
    std::vector<int32_t> values;
    for (uint32_t i = 0; i < view.count(); ++i) {
        if (view.isNull(i)) throw arrayError(std::string(what) + " cannot be null");
        values.push_back(load32(view.fixed(i)));
    }
    return values;
}

using Numbers = NumberArray;

double powerOf10(uint32_t n)
{
    double p = 1;
    while (n-- > 0) p *= 10;
    return p;
}

/// \returns The number (integer, bigint, numeric or double precision) at \p p as a double
double numberAt(const uint8_t * p, SqlType type)
{
    switch (type.typeID) {
        case TypeID::IntegerID: return load32(p);
        case TypeID::LongIntegerID: return static_cast<double>(load64(p));
        case TypeID::NumericID: return static_cast<double>(load64(p)) / powerOf10(type.numericLayout.precision);
        case TypeID::DoubleID: return loadDouble(p);
        default: throw arrayError("linear algebra takes numbers, not " + ::Sql::getName(::Sql::toNotNullableTy(type)));
    }
}

Numbers numbersOf(Handle handle, SqlType elementType)
{
    View view(handle, elementType);
    Numbers n;
    const uint32_t count = view.count();
    if (count == 0) return n;
    for (uint32_t d = 0; d < view.dims(); ++d) { n.widths.push_back(view.width(d)); n.lowers.push_back(view.lower(d)); }
    n.values.resize(count);
    if (elementType.typeID == TypeID::DoubleID) {
        std::memcpy(n.values.data(), view.fixed(0), 8 * size_t(count)); // a NULL element has zero bytes
    } else {
        for (uint32_t i = 0; i < count; ++i) if (!view.isNull(i)) n.values[i] = numberAt(view.fixed(i), elementType);
    }
    if (view.anyNull()) {
        n.nulls.resize(count);
        for (uint32_t i = 0; i < count; ++i) n.nulls[i] = view.isNull(i);
    }
    return n;
}

/// \returns The elements (followed by the zeroed NULL bitmap) of a new representation of the shape in the arena, its handle in \p handle
uint8_t * allocateFixed(const std::vector<int32_t> & widths, const std::vector<int32_t> & lowers, size_t count, size_t elementSize,
                        Arena * arena, Handle & handle)
{
    const uint32_t dims = count ? static_cast<uint32_t>(widths.size()) : 0;
    for (uint32_t d = 0; d < dims; ++d) {
        if (static_cast<int64_t>(lowers[d]) + widths[d] - 1 > INT32_MAX) throw arrayError("array upper bound is too large");
    }
    if (count > INT32_MAX) throw arrayError("array size exceeds the maximum allowed");
    const size_t size = 4 + 8 * size_t(dims) + elementSize * count + nullBytes(static_cast<uint32_t>(count));
    uint8_t * out = arena->allocate(size), * w = out;
    std::memcpy(w, &dims, 4); w += 4;
    if (dims) {
        std::memcpy(w, widths.data(), 4 * dims); w += 4 * dims;
        std::memcpy(w, lowers.data(), 4 * dims); w += 4 * dims;
    }
    std::memset(w + elementSize * count, 0, nullBytes(static_cast<uint32_t>(count)));
    handle = Handle{out, size | Handle::temporary};
    return w;
}

/// \returns An array of double precision of the shape with the values (NULL where \p nulls, if any, says)
Handle doubles(const std::vector<int32_t> & widths, const std::vector<int32_t> & lowers, const std::vector<double> & values,
               const std::vector<uint8_t> & nulls, Arena * arena)
{
    if (arena == nullptr) {
        Builder builder(::Sql::getDoubleTy());
        builder.widths = widths;
        builder.lowers = lowers;
        for (size_t i = 0; i < values.size(); ++i) {
            if (!nulls.empty() && nulls[i]) builder.addNull();
            else builder.addFixed(&values[i]);
        }
        return builder.finish(arena);
    }
    Handle handle;
    const size_t count = values.size();
    uint8_t * elements = allocateFixed(widths, lowers, count, 8, arena, handle);
    if (count) std::memcpy(elements, values.data(), 8 * count);
    if (!nulls.empty()) {
        uint8_t * bitmap = elements + 8 * count;
        for (size_t i = 0; i < count; ++i) {
            if (!nulls[i]) continue;
            std::memset(elements + 8 * i, 0, 8);
            bitmap[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
        }
    }
    return handle;
}

void writeDouble(void * result, double value) { std::memcpy(result, &value, 8); }

double elementwise(ElementwiseOp op, double x, double y)
{
    switch (op) {
        case ElementwiseOp::Add: return x + y;
        case ElementwiseOp::Sub: return x - y;
        case ElementwiseOp::Mul: return x * y;
        case ElementwiseOp::Div:
            if (y == 0) throw arrayError("division by zero");
            return x / y;
        case ElementwiseOp::Pow: return std::pow(x, y);
    }
    return 0;
}

double mapNumber(MapOp op, double x)
{
    switch (op) {
        case MapOp::Abs: return std::fabs(x);
        case MapOp::Sig: return 1.0 / (1.0 + std::exp(-x));
        case MapOp::Relu: return x > 0 ? x : 0;
        case MapOp::ReluP: return x > 0 ? 1 : 0;
        case MapOp::Tanh: return std::tanh(x);
    }
    return 0;
}

/// C (m x n) = A (m x k) B (k x n), row-major, in blocks that stay in the caches; the innermost loop runs
/// over a row of B and of C, which the compiler vectorises
void multiply(const double * a, const double * b, double * c, size_t m, size_t k, size_t n)
{
    std::fill(c, c + m * n, 0.0);
    constexpr size_t blockM = 64, blockK = 256, blockN = 512;
    for (size_t i0 = 0; i0 < m; i0 += blockM) {
        const size_t i1 = std::min(m, i0 + blockM);
        for (size_t k0 = 0; k0 < k; k0 += blockK) {
            const size_t k1 = std::min(k, k0 + blockK);
            for (size_t j0 = 0; j0 < n; j0 += blockN) {
                const size_t j1 = std::min(n, j0 + blockN);
                for (size_t i = i0; i < i1; ++i) {
                    double * row = c + i * n;
                    for (size_t l = k0; l < k1; ++l) {
                        const double x = a[i * k + l];
                        const double * other = b + l * n;
                        for (size_t j = j0; j < j1; ++j) row[j] += x * other[j];
                    }
                }
            }
        }
    }
}

/// The exponentials of x - max over all elements and their sum (the shift keeps them finite)
double shiftedExponentials(const Numbers & x, std::vector<double> & out)
{
    double max = -std::numeric_limits<double>::infinity();
    for (double v : x.values) max = std::max(max, v);
    out.resize(x.count());
    double sum = 0;
    for (size_t i = 0; i < x.count(); ++i) {
        out[i] = std::exp(x.values[i] - max);
        sum += out[i];
    }
    return sum;
}

/// \returns The shift x - max - log(sum of exp(x - max)), the logarithms of softmax(x)
std::vector<double> logSoftmax(const Numbers & x)
{
    double max = -std::numeric_limits<double>::infinity();
    for (double v : x.values) max = std::max(max, v);
    std::vector<double> e;
    const double logSum = std::log(shiftedExponentials(x, e));
    std::vector<double> out(x.count());
    for (size_t i = 0; i < x.count(); ++i) out[i] = x.values[i] - max - logSum;
    return out;
}

/// \returns Whether an argument is an array (otherwise a number)
bool isArrayArgument(const Args & a, uint32_t i) { return a.type(i).typeID == TypeID::ArrayID; }

/// \returns Whether the result is NULL
bool call(Function function, Arena * arena, SqlType resultType, void * result, const Args & a)
{
    auto elementTypeOf = [&](uint32_t i) { return ::Sql::getElementTy(a.type(i)); };
    switch (function) {
        case Function::Construct: {
            SqlType elementType = ::Sql::getElementTy(resultType);
            Builder builder(elementType);
            if (a.argc > 0 && a.type(0).typeID == TypeID::ArrayID) {
                // of arrays: one dimension more, all of the same shape
                std::vector<View> parts;
                for (uint32_t i = 0; i < a.argc; ++i) {
                    if (a.null(i)) throw arrayError("cannot construct an array of NULL sub-arrays");
                    parts.emplace_back(a.array(i), elementType);
                }
                bool allEmpty = std::all_of(parts.begin(), parts.end(), [](const View & v) { return v.count() == 0; });
                if (!allEmpty) {
                    for (auto & p : parts) {
                        bool same = p.dims() == parts[0].dims();
                        for (uint32_t d = 0; same && d < p.dims(); ++d) same = p.width(d) == parts[0].width(d) && p.lower(d) == parts[0].lower(d);
                        if (!same) throw arrayError("multidimensional arrays must have array expressions with matching dimensions");
                    }
                    builder.widths.push_back(static_cast<int32_t>(parts.size()));
                    builder.lowers.push_back(1);
                    for (uint32_t d = 0; d < parts[0].dims(); ++d) {
                        builder.widths.push_back(parts[0].width(d));
                        builder.lowers.push_back(parts[0].lower(d));
                    }
                    for (auto & p : parts) for (uint32_t i = 0; i < p.count(); ++i) builder.add(p, i);
                }
            } else {
                if (a.argc > 0) {
                    builder.widths.push_back(static_cast<int32_t>(a.argc));
                    builder.lowers.push_back(1);
                }
                for (uint32_t i = 0; i < a.argc; ++i) addElement(builder, elementOf(a, i, elementType), elementType);
            }
            storeHandle(result, builder.finish(arena));
            return false;
        }
        case Function::Subscript: {
            if (a.null(0)) return true;
            Handle h = a.array(0);
            View view(h, elementTypeOf(0));
            std::vector<int32_t> index;
            for (uint32_t i = 1; i < a.argc; ++i) {
                if (a.null(i)) return true;
                index.push_back(a.integer(i));
            }
            int64_t position = subscriptIndex(view, index);
            if (position < 0 || view.isNull(static_cast<uint32_t>(position))) return true;
            writeElement(view, static_cast<uint32_t>(position), h, result);
            return false;
        }
        case Function::Slice: {
            if (a.null(0)) return true;
            SqlType elementType = elementTypeOf(0);
            View view(a.array(0), elementType);
            uint32_t given = (a.argc - 1) / 2;
            for (uint32_t i = 1; i < a.argc; ++i) if (a.null(i)) return true;
            Builder builder(elementType);
            if (given > view.dims() || view.count() == 0) {
                storeHandle(result, builder.finish(arena));
                return false;
            }
            std::vector<int32_t> from(view.dims()), to(view.dims());
            bool empty = false;
            for (uint32_t d = 0; d < view.dims(); ++d) {
                int64_t l = view.lower(d), u = view.upper(d);
                if (d < given) {
                    int32_t lo = a.integer(1 + 2 * d), hi = a.integer(2 + 2 * d);
                    if (lo != INT32_MIN) l = std::max<int64_t>(l, lo);
                    if (hi != INT32_MAX) u = std::min<int64_t>(u, hi);
                }
                if (l > u) empty = true;
                from[d] = static_cast<int32_t>(l);
                to[d] = static_cast<int32_t>(u);
            }
            if (!empty) {
                for (uint32_t d = 0; d < view.dims(); ++d) {
                    builder.widths.push_back(to[d] - from[d] + 1);
                    builder.lowers.push_back(1);
                }
                std::vector<int32_t> index(from);
                for (;;) {
                    builder.add(view, static_cast<uint32_t>(subscriptIndex(view, index)));
                    uint32_t d = view.dims();
                    while (d > 0 && ++index[d - 1] > to[d - 1]) { index[d - 1] = from[d - 1]; --d; }
                    if (d == 0) break;
                }
            }
            storeHandle(result, builder.finish(arena));
            return false;
        }
        case Function::Append:
        case Function::Prepend: {
            uint32_t arrayArg = function == Function::Append ? 0 : 1, elementArg = 1 - arrayArg;
            SqlType elementType = ::Sql::getElementTy(resultType);
            Builder builder(elementType);
            Element e = elementOf(a, elementArg, elementType);
            if (a.null(arrayArg)) {
                builder.widths = {1};
                builder.lowers = {1};
                addElement(builder, e, elementType);
            } else {
                View view(a.array(arrayArg), elementType);
                if (view.dims() > 1) throw arrayError("argument must be empty or one-dimensional array");
                builder.widths = {static_cast<int32_t>(view.count()) + 1};
                builder.lowers = {view.count() ? view.lower(0) : 1};
                if (function == Function::Prepend) addElement(builder, e, elementType);
                for (uint32_t i = 0; i < view.count(); ++i) builder.add(view, i);
                if (function == Function::Append) addElement(builder, e, elementType);
            }
            storeHandle(result, builder.finish(arena));
            return false;
        }
        case Function::Cat: {
            if (a.null(0) && a.null(1)) return true;
            SqlType elementType = ::Sql::getElementTy(resultType);
            if (a.null(0)) { storeHandle(result, a.array(1)); return false; }
            if (a.null(1)) { storeHandle(result, a.array(0)); return false; }
            storeHandle(result, concatenate(a.array(0), a.array(1), elementType, arena));
            return false;
        }
        case Function::Length:
        case Function::Lower:
        case Function::Upper: {
            if (a.null(0) || a.null(1)) return true;
            View view(a.array(0), elementTypeOf(0));
            int32_t d = a.integer(1);
            if (view.count() == 0 || d < 1 || static_cast<uint32_t>(d) > view.dims()) return true;
            uint32_t dim = static_cast<uint32_t>(d - 1);
            writeInteger(result, function == Function::Length ? view.width(dim) : function == Function::Lower ? view.lower(dim) : view.upper(dim));
            return false;
        }
        case Function::Ndims: {
            if (a.null(0)) return true;
            View view(a.array(0), elementTypeOf(0));
            if (view.count() == 0) return true;
            writeInteger(result, static_cast<int32_t>(view.dims()));
            return false;
        }
        case Function::Dims: {
            if (a.null(0)) return true;
            View view(a.array(0), elementTypeOf(0));
            if (view.count() == 0) return true;
            std::string s;
            for (uint32_t d = 0; d < view.dims(); ++d) s += "[" + std::to_string(view.lower(d)) + ":" + std::to_string(view.upper(d)) + "]";
            writeText(result, s);
            return false;
        }
        case Function::Cardinality: {
            if (a.null(0)) return true;
            writeInteger(result, static_cast<int32_t>(View(a.array(0), elementTypeOf(0)).count()));
            return false;
        }
        case Function::Position:
        case Function::Positions: {
            if (a.null(0)) return true;
            SqlType elementType = elementTypeOf(0);
            View view(a.array(0), elementType);
            requireOneDimension(view, "searching for elements");
            Handle probe = single(elementOf(a, 1, elementType), elementType, arena);
            View e(probe, elementType);
            if (function == Function::Position) {
                uint32_t first = 0;
                if (a.argc > 2) {
                    if (a.null(2)) throw arrayError("initial position must not be null");
                    int64_t start = a.integer(2);
                    if (view.count() > 0) first = static_cast<uint32_t>(std::max<int64_t>(0, start - view.lower(0)));
                }
                for (uint32_t i = first; i < view.count(); ++i) {
                    if (notDistinct(e, 0, view, i)) { writeInteger(result, view.lower(0) + static_cast<int32_t>(i)); return false; }
                }
                return true;
            }
            Builder builder(::Sql::getIntegerTy());
            for (uint32_t i = 0; i < view.count(); ++i) {
                if (notDistinct(e, 0, view, i)) {
                    int32_t position = view.lower(0) + static_cast<int32_t>(i);
                    builder.addFixed(&position);
                }
            }
            builder.widths = {static_cast<int32_t>(builder.count())};
            builder.lowers = {1};
            storeHandle(result, builder.finish(arena));
            return false;
        }
        case Function::Remove:
        case Function::Replace: {
            if (a.null(0)) return true;
            SqlType elementType = elementTypeOf(0);
            View view(a.array(0), elementType);
            if (function == Function::Remove) requireOneDimension(view, "removing elements");
            Handle probe = single(elementOf(a, 1, elementType), elementType, arena);
            View e(probe, elementType);
            Element replacement = function == Function::Replace ? elementOf(a, 2, elementType) : Element();
            Builder builder(elementType);
            for (uint32_t i = 0; i < view.count(); ++i) {
                if (!notDistinct(e, 0, view, i)) builder.add(view, i);
                else if (function == Function::Replace) addElement(builder, replacement, elementType);
            }
            if (function == Function::Remove) {
                if (builder.count() > 0) { builder.widths = {static_cast<int32_t>(builder.count())}; builder.lowers = {view.lower(0)}; }
            } else {
                for (uint32_t d = 0; d < view.dims(); ++d) { builder.widths.push_back(view.width(d)); builder.lowers.push_back(view.lower(d)); }
            }
            storeHandle(result, builder.finish(arena));
            return false;
        }
        case Function::Fill: {
            if (a.null(1)) throw arrayError("dimension array or low bound array cannot be null");
            SqlType elementType = ::Sql::getElementTy(resultType);
            std::vector<int32_t> widths = integerList(a.array(1), "dimension values");
            std::vector<int32_t> lowers(widths.size(), 1);
            if (a.argc > 2) {
                if (a.null(2)) throw arrayError("dimension array or low bound array cannot be null");
                lowers = integerList(a.array(2), "low bound values");
                if (lowers.size() != widths.size()) throw arrayError("wrong number of array subscripts: low bound array has different size than dimensions array");
            }
            if (widths.size() > 6) throw arrayError("number of array dimensions exceeds the maximum allowed (6)");
            Builder builder(elementType);
            uint64_t count = widths.empty() ? 0 : 1;
            for (int32_t w : widths) {
                if (w < 0) throw arrayError("array size must be positive");
                count *= static_cast<uint64_t>(w);
                if (count > 100000000) throw arrayError("array size exceeds the maximum allowed");
            }
            if (count > 0) {
                builder.widths = widths;
                builder.lowers = lowers;
                Element e = elementOf(a, 0, elementType);
                for (uint64_t i = 0; i < count; ++i) addElement(builder, e, elementType);
            }
            storeHandle(result, builder.finish(arena));
            return false;
        }
        case Function::ToString: {
            if (a.null(0) || a.null(1)) return true;
            View view(a.array(0), elementTypeOf(0));
            std::string delimiter = a.text(1);
            bool withNulls = a.argc > 2 && !a.null(2);
            std::string nullString = withNulls ? a.text(2) : std::string();
            std::string out;
            bool first = true;
            for (uint32_t i = 0; i < view.count(); ++i) {
                if (view.isNull(i) && !withNulls) continue;
                if (!first) out += delimiter;
                first = false;
                out += view.isNull(i) ? nullString : formatElement(view, i);
            }
            writeText(result, out);
            return false;
        }
        case Function::FromString: {
            if (a.null(0)) return true;
            SqlType elementType = ::Sql::getElementTy(resultType);
            std::string input = a.text(0);
            bool withNulls = a.argc > 2 && !a.null(2);
            std::string nullString = withNulls ? a.text(2) : std::string();
            std::vector<std::string> parts;
            if (!input.empty()) {
                if (a.null(1)) {
                    for (char c : input) parts.emplace_back(1, c); // each character
                } else {
                    std::string delimiter = a.text(1);
                    if (delimiter.empty()) {
                        parts.push_back(input);
                    } else {
                        size_t begin = 0;
                        for (;;) {
                            size_t end = input.find(delimiter, begin);
                            if (end == std::string::npos) { parts.push_back(input.substr(begin)); break; }
                            parts.push_back(input.substr(begin, end - begin));
                            begin = end + delimiter.size();
                        }
                    }
                }
            }
            Builder builder(elementType);
            for (auto & p : parts) {
                if (withNulls && p == nullString) builder.addNull();
                else addParsed(builder, elementType, p);
            }
            if (!parts.empty()) { builder.widths = {static_cast<int32_t>(parts.size())}; builder.lowers = {1}; }
            storeHandle(result, builder.finish(arena));
            return false;
        }
        case Function::Trim: {
            if (a.null(0) || a.null(1)) return true;
            SqlType elementType = elementTypeOf(0);
            View view(a.array(0), elementType);
            int32_t n = a.integer(1);
            int32_t first = view.count() ? view.width(0) : 0;
            if (n < 0 || n > first) throw arrayError("number of elements to trim must be between 0 and " + std::to_string(first));
            Builder builder(elementType);
            if (n < first) {
                for (uint32_t d = 0; d < view.dims(); ++d) { builder.widths.push_back(view.width(d)); builder.lowers.push_back(view.lower(d)); }
                builder.widths[0] -= n;
                uint32_t keep = view.count() / static_cast<uint32_t>(first) * static_cast<uint32_t>(first - n);
                for (uint32_t i = 0; i < keep; ++i) builder.add(view, i);
            }
            storeHandle(result, builder.finish(arena));
            return false;
        }
        case Function::Contains:
        case Function::ContainedBy:
        case Function::Overlaps: {
            if (a.null(0) || a.null(1)) return true;
            SqlType elementType = elementTypeOf(0);
            View first(a.array(0), elementType), second(a.array(1), elementType);
            const View & x = function == Function::ContainedBy ? second : first;
            const View & y = function == Function::ContainedBy ? first : second;
            if (function == Function::Overlaps) {
                for (uint32_t i = 0; i < x.count(); ++i) {
                    if (x.isNull(i)) continue;
                    for (uint32_t j = 0; j < y.count(); ++j) {
                        if (!y.isNull(j) && compareElements(x, i, y, j) == 0) { writeBool(result, true); return false; }
                    }
                }
                writeBool(result, false);
                return false;
            }
            // x contains every element of y; NULLs match nothing
            for (uint32_t j = 0; j < y.count(); ++j) {
                bool found = false;
                if (!y.isNull(j)) {
                    for (uint32_t i = 0; i < x.count() && !found; ++i) found = !x.isNull(i) && compareElements(x, i, y, j) == 0;
                }
                if (!found) { writeBool(result, false); return false; }
            }
            writeBool(result, true);
            return false;
        }
        case Function::AnyCompare:
        case Function::AllCompare: {
            if (a.null(1)) return true;
            SqlType elementType = elementTypeOf(1);
            View view(a.array(1), elementType);
            uint32_t op = static_cast<uint32_t>(a.integer(2));
            Handle probe = single(elementOf(a, 0, elementType), elementType, arena);
            View e(probe, elementType);
            bool any = function == Function::AnyCompare, sawNull = false;
            for (uint32_t i = 0; i < view.count(); ++i) {
                int r = compareWith(e, view, i, op);
                if (r < 0) { sawNull = true; continue; }
                if (any && r == 1) { writeBool(result, true); return false; }
                if (!any && r == 0) { writeBool(result, false); return false; }
            }
            if (sawNull) return true;
            writeBool(result, !any);
            return false;
        }
        case Function::Equals: {
            if (a.null(0) || a.null(1)) return true;
            writeBool(result, equals(a.array(0), a.array(1), a.type(0)));
            return false;
        }
        case Function::Elementwise: {
            if (a.null(0) || a.null(1) || a.null(2)) return true;
            const auto op = static_cast<ElementwiseOp>(a.integer(2));
            const bool leftArray = isArrayArgument(a, 0), rightArray = isArrayArgument(a, 1);
            if (!leftArray && !rightArray) {
                writeDouble(result, elementwise(op, numberAt(static_cast<const uint8_t *>(a.args[0]), a.type(0)),
                                                numberAt(static_cast<const uint8_t *>(a.args[1]), a.type(1))));
                return false;
            }
            Numbers x, y;
            double scalar = 0;
            if (leftArray) x = numbersOf(a.array(0), elementTypeOf(0));
            else scalar = numberAt(static_cast<const uint8_t *>(a.args[0]), a.type(0));
            if (rightArray) y = numbersOf(a.array(1), elementTypeOf(1));
            else scalar = numberAt(static_cast<const uint8_t *>(a.args[1]), a.type(1));
            if (leftArray && rightArray && (x.widths != y.widths)) {
                throw arrayError("arrays of different shapes: " + std::to_string(x.widths.size()) + " and " + std::to_string(y.widths.size()) +
                                 " dimensions or different lengths");
            }
            // in place of the array (of the left, if both are)
            Numbers & out = leftArray ? x : y;
            if (leftArray && rightArray && y.anyNull()) {
                if (!x.anyNull()) x.nulls.assign(x.count(), 0);
                for (size_t i = 0; i < x.count(); ++i) x.nulls[i] |= y.nulls[i];
            }
            for (size_t i = 0; i < out.count(); ++i) {
                if (out.isNull(i)) continue;
                out.values[i] = elementwise(op, leftArray ? x.values[i] : scalar, rightArray ? y.values[i] : scalar);
            }
            storeHandle(result, doubles(out.widths, out.lowers, out.values, out.nulls, arena));
            return false;
        }
        case Function::MatrixProduct: {
            if (a.null(0) || a.null(1)) return true;
            Numbers x = numbersOf(a.array(0), elementTypeOf(0)), y = numbersOf(a.array(1), elementTypeOf(1));
            if (x.widths.empty() || y.widths.empty() || x.widths.back() != y.widths.front()) {
                throw arrayError("arrays do not have suitable size for **: the last dimension of the first (" +
                                 (x.widths.empty() ? std::string("empty") : std::to_string(x.widths.back())) +
                                 ") differs from the first dimension of the second (" +
                                 (y.widths.empty() ? std::string("empty") : std::to_string(y.widths.front())) + ")");
            }
            const size_t k = static_cast<size_t>(x.widths.back()), m = x.count() / std::max<size_t>(k, 1), n = y.count() / std::max<size_t>(k, 1);
            std::vector<int32_t> widths(x.widths.begin(), x.widths.end() - 1), lowers(x.lowers.begin(), x.lowers.end() - 1);
            widths.insert(widths.end(), y.widths.begin() + 1, y.widths.end());
            lowers.insert(lowers.end(), y.lowers.begin() + 1, y.lowers.end());
            if (widths.empty()) { widths = {1}; lowers = {1}; } // two vectors: their dot product
            std::vector<double> values(m * n);
            multiply(x.values.data(), y.values.data(), values.data(), m, k, n);
            // a NULL in a row of the first or a column of the second makes the sums over it NULL
            std::vector<uint8_t> nulls;
            if (x.anyNull() || y.anyNull()) {
                std::vector<uint8_t> rowNull(m), columnNull(n);
                for (size_t i = 0; i < m; ++i) for (size_t l = 0; l < k; ++l) if (x.isNull(i * k + l)) rowNull[i] = true;
                for (size_t l = 0; l < k; ++l) for (size_t j = 0; j < n; ++j) if (y.isNull(l * n + j)) columnNull[j] = true;
                nulls.resize(m * n);
                for (size_t i = 0; i < m; ++i) for (size_t j = 0; j < n; ++j) nulls[i * n + j] = rowNull[i] || columnNull[j];
            }
            storeHandle(result, doubles(widths, lowers, values, nulls, arena));
            return false;
        }
        case Function::Kronecker: {
            if (a.null(0) || a.null(1)) return true;
            Numbers x = numbersOf(a.array(0), elementTypeOf(0)), y = numbersOf(a.array(1), elementTypeOf(1));
            if (x.widths.size() != 2 || y.widths.size() != 2) throw arrayError("arrays for kronecker product need two dimensions");
            const size_t r1 = x.widths[0], c1 = x.widths[1], r2 = y.widths[0], c2 = y.widths[1], columns = c1 * c2;
            std::vector<double> values(r1 * r2 * columns);
            std::vector<uint8_t> nulls(x.anyNull() || y.anyNull() ? values.size() : 0);
            for (size_t i = 0; i < r1; ++i)
                for (size_t k = 0; k < r2; ++k)
                    for (size_t j = 0; j < c1; ++j)
                        for (size_t l = 0; l < c2; ++l) {
                            const size_t to = (i * r2 + k) * columns + j * c2 + l, p = i * c1 + j, q = k * c2 + l;
                            if (!nulls.empty()) nulls[to] = x.isNull(p) || y.isNull(q);
                            values[to] = x.values[p] * y.values[q];
                        }
            storeHandle(result, doubles({static_cast<int32_t>(r1 * r2), static_cast<int32_t>(columns)}, {1, 1}, values, nulls, arena));
            return false;
        }
        case Function::Transpose: {
            if (a.null(0)) return true;
            SqlType elementType = elementTypeOf(0);
            View view(a.array(0), elementType);
            Builder builder(elementType);
            if (view.count() == 0) { storeHandle(result, builder.finish(arena)); return false; }
            // a vector is a row of one matrix: its transpose a column
            std::vector<int32_t> widths, lowers;
            for (uint32_t d = 0; d < view.dims(); ++d) { widths.push_back(view.width(d)); lowers.push_back(view.lower(d)); }
            if (widths.size() == 1) { widths.insert(widths.begin(), 1); lowers.insert(lowers.begin(), 1); }
            const size_t rows = widths[0], columns = widths[1], inner = view.count() / (rows * columns);
            builder.widths = {widths[1], widths[0]};
            builder.lowers = {lowers[1], lowers[0]};
            for (size_t d = 2; d < widths.size(); ++d) { builder.widths.push_back(widths[d]); builder.lowers.push_back(lowers[d]); }
            if (arena != nullptr && !isStringElement(elementType)) {
                // the bytes of the elements, without a builder
                Handle handle;
                const size_t size = elementSize(elementType);
                uint8_t * elements = allocateFixed(builder.widths, builder.lowers, view.count(), size, arena, handle), * to = elements;
                uint8_t * bitmap = elements + size * view.count();
                for (size_t j = 0, t = 0; j < columns; ++j)
                    for (size_t i = 0; i < rows; ++i)
                        for (size_t e = 0; e < inner; ++e, ++t, to += size) {
                            const auto from = static_cast<uint32_t>((i * columns + j) * inner + e);
                            std::memcpy(to, view.fixed(from), size);
                            if (view.isNull(from)) bitmap[t >> 3] |= static_cast<uint8_t>(1u << (t & 7));
                        }
                storeHandle(result, handle);
                return false;
            }
            for (size_t j = 0; j < columns; ++j)
                for (size_t i = 0; i < rows; ++i)
                    for (size_t e = 0; e < inner; ++e) builder.add(view, static_cast<uint32_t>((i * columns + j) * inner + e));
            storeHandle(result, builder.finish(arena));
            return false;
        }
        case Function::Map: {
            if (a.null(0) || a.null(1)) return true;
            const auto op = static_cast<MapOp>(a.integer(1));
            if (!isArrayArgument(a, 0)) {
                writeDouble(result, mapNumber(op, numberAt(static_cast<const uint8_t *>(a.args[0]), a.type(0))));
                return false;
            }
            Numbers x = numbersOf(a.array(0), elementTypeOf(0));
            for (size_t i = 0; i < x.count(); ++i) if (!x.isNull(i)) x.values[i] = mapNumber(op, x.values[i]);
            storeHandle(result, doubles(x.widths, x.lowers, x.values, x.nulls, arena));
            return false;
        }
        case Function::Softmax:
        case Function::SoftmaxCce:
        case Function::SoftmaxCceP:
        case Function::HighestPosition: {
            // over all elements: NULL if one of them is NULL
            if (a.null(0) || (a.argc > 1 && a.null(1))) return true;
            Numbers x = numbersOf(a.array(0), elementTypeOf(0)), labels;
            if (x.anyNull()) return true;
            if (a.argc > 1) {
                labels = numbersOf(a.array(1), elementTypeOf(1));
                if (labels.anyNull()) return true;
                if (labels.count() != x.count()) {
                    throw arrayError("arrays do not have suitable size: " + std::to_string(x.count()) + " values and " +
                                     std::to_string(labels.count()) + " labels");
                }
            }
            if (function == Function::HighestPosition) {
                if (x.count() == 0) return true;
                size_t best = 0;
                for (size_t i = 1; i < x.count(); ++i) if (x.values[i] > x.values[best]) best = i;
                writeInteger(result, static_cast<int32_t>(best));
                return false;
            }
            if (function == Function::SoftmaxCce) {
                if (x.count() == 0) return true;
                const auto logs = logSoftmax(x);
                double loss = 0;
                for (size_t i = 0; i < x.count(); ++i) loss -= labels.values[i] * logs[i];
                writeDouble(result, loss);
                return false;
            }
            std::vector<double> e;
            const double sum = shiftedExponentials(x, e);
            for (size_t i = 0; i < x.count(); ++i) {
                e[i] /= sum;
                if (function == Function::SoftmaxCceP) e[i] -= labels.values[i];
            }
            storeHandle(result, doubles(x.widths, x.lowers, e, {}, arena));
            return false;
        }
    }
    throw arrayError("unknown array function");
}
}

NumberArray toNumbers(Handle handle, SqlType elementType)
{
    return numbersOf(handle, elementType);
}

void addNumbers(NumberArray & sum, Handle handle, SqlType elementType)
{
    View view(handle, elementType);
    const uint32_t dims = view.count() ? view.dims() : 0;
    bool same = dims == sum.widths.size();
    for (uint32_t d = 0; same && d < dims; ++d) same = view.width(d) == sum.widths[d];
    if (!same) throw arrayError("sum of arrays of different shapes");
    if (elementType.typeID == TypeID::DoubleID) {
        for (uint32_t i = 0; i < view.count(); ++i) sum.values[i] += loadDouble(view.fixed(i));
    } else {
        for (uint32_t i = 0; i < view.count(); ++i) if (!view.isNull(i)) sum.values[i] += numberAt(view.fixed(i), elementType);
    }
    if (view.anyNull()) {
        if (!sum.anyNull()) sum.nulls.assign(sum.count(), 0);
        for (uint32_t i = 0; i < view.count(); ++i) if (view.isNull(i)) sum.nulls[i] = 1;
    }
}

void addNumbers(NumberArray & sum, const NumberArray & add)
{
    if (add.widths != sum.widths) throw arrayError("sum of arrays of different shapes");
    for (size_t i = 0; i < add.count(); ++i) sum.values[i] += add.values[i];
    if (add.anyNull()) {
        if (!sum.anyNull()) sum.nulls.assign(sum.count(), 0);
        for (size_t i = 0; i < add.count(); ++i) sum.nulls[i] |= add.nulls[i];
    }
}

Handle fromNumbers(const NumberArray & numbers, Arena * arena)
{
    return doubles(numbers.widths, numbers.lowers, numbers.values, numbers.nulls, arena);
}

extern "C" bool tardisArrayCall(uint32_t function, Arena * arena, uint64_t resultType, void * result,
                                uint32_t argc, void * const * args, const bool * nulls, const uint64_t * types)
{
    Args a{argc, args, nulls, types};
    return call(static_cast<Function>(function), arena, ::Sql::toNotNullableTy(typeOfBits(resultType)), result, a);
}

extern "C" bool tardisArrayEquals(const void * a, const void * b, uint64_t arrayType)
{
    return equals(loadHandle(a), loadHandle(b), typeOfBits(arrayType));
}

extern "C" int32_t tardisArrayCompare(const void * a, const void * b, uint64_t arrayType)
{
    return compare(loadHandle(a), loadHandle(b), typeOfBits(arrayType));
}

extern "C" uint64_t tardisArrayHash(const void * a, uint64_t arrayType)
{
    return hash(loadHandle(a), typeOfBits(arrayType));
}

} // end namespace Arrays
} // end namespace Sql
} // end namespace Native
