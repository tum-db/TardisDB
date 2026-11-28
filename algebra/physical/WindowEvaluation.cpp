#include "algebra/physical/WindowEvaluation.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>

#include "foundations/Parallel.hpp"
#include "sql/SqlValues.hpp"

namespace Algebra {
namespace Physical {
namespace WindowEvaluation {

using ID = Sql::SqlType::TypeID;

namespace {

constexpr size_t hashGroups = 1024; // a power of two, few enough for partitioning in the caches
constexpr size_t fanout = 16;       // of the segment trees
constexpr size_t parallelRows = 20000; // fewer rows run on one thread

//-----------------------------------------------------------------------------
// the values of rows

bool isNull(const uint8_t * row, const Field & f)
{
    return f.nullOffset != SIZE_MAX && row[f.nullOffset] != 0;
}

void setNull(uint8_t * row, const Field & f, bool null)
{
    if (f.nullOffset != SIZE_MAX) row[f.nullOffset] = null ? 1 : 0;
}

bool isIntegerLike(ID id)
{
    return id == ID::IntegerID || id == ID::LongIntegerID || id == ID::NumericID || id == ID::DateID || id == ID::TimestampID || id == ID::BoolID;
}

int64_t readInteger(const uint8_t * row, const Field & f)
{
    const uint8_t * p = row + f.offset;
    switch (f.type.typeID) {
        case ID::BoolID: return *p != 0;
        case ID::IntegerID:
        case ID::DateID: { int32_t v; std::memcpy(&v, p, 4); return v; }
        default: { int64_t v; std::memcpy(&v, p, 8); return v; }
    }
}

double readDouble(const uint8_t * row, const Field & f)
{
    double v;
    std::memcpy(&v, row + f.offset, 8);
    return v;
}

struct Bytes {
    const uint8_t * data;
    size_t length;
};

/// The bytes of a string (of a char without its trailing spaces)
Bytes readString(const uint8_t * row, const Field & f)
{
    const uint8_t * p = row + f.offset;
    Bytes b{p, 0};
    if (f.type.typeID == ID::TextID) {
        char * text = reinterpret_cast<char *>(const_cast<uint8_t *>(p));
        return {static_cast<const uint8_t *>(Sql::textBegin(text)), static_cast<size_t>(Sql::getLengthText(text))};
    }
    if (f.type.typeID == ID::CharID && f.type.length == 1) {
        b.length = 1;
    } else {
        const size_t prefix = f.type.length < 256 ? 1 : f.type.length < 65536 ? 2 : 4;
        uint32_t length = 0;
        std::memcpy(&length, p, prefix); // little endian
        b = {p + prefix, std::min<size_t>(length, f.type.length)};
    }
    if (f.type.typeID == ID::CharID) while (b.length > 0 && (b.data[b.length - 1] == ' ' || b.data[b.length - 1] == 0)) --b.length;
    return b;
}

bool isString(ID id)
{
    return id == ID::TextID || id == ID::CharID || id == ID::VarcharID;
}

/// \returns <0, 0 or >0: the order of two values that are not NULL
int compareValues(const uint8_t * a, const uint8_t * b, const Field & f)
{
    const ID id = f.type.typeID;
    if (id == ID::DoubleID) {
        const double x = readDouble(a, f), y = readDouble(b, f);
        return x < y ? -1 : x > y ? 1 : 0;
    }
    if (isString(id)) {
        const Bytes x = readString(a, f), y = readString(b, f);
        const int c = std::memcmp(x.data, y.data, std::min(x.length, y.length));
        if (c != 0) return c;
        return x.length < y.length ? -1 : x.length > y.length ? 1 : 0;
    }
    const int64_t x = readInteger(a, f), y = readInteger(b, f);
    return x < y ? -1 : x > y ? 1 : 0;
}

/// NULL equals NULL
bool equalValues(const uint8_t * a, const uint8_t * b, const Field & f)
{
    const bool aNull = isNull(a, f), bNull = isNull(b, f);
    if (aNull || bNull) return aNull == bNull;
    return compareValues(a, b, f) == 0;
}

uint64_t mix(uint64_t h)
{
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

/// Equal values (as equalValues) have equal hashes
uint64_t hashValue(const uint8_t * row, const Field & f)
{
    if (isNull(row, f)) return 0x9e3779b97f4a7c15ULL;
    const ID id = f.type.typeID;
    if (id == ID::DoubleID) {
        double v = readDouble(row, f);
        if (v == 0) v = 0; // -0 equals 0
        uint64_t bits;
        std::memcpy(&bits, &v, 8);
        return mix(bits);
    }
    if (isString(id)) {
        const Bytes b = readString(row, f);
        uint64_t h = 1469598103934665603ULL;
        for (size_t i = 0; i < b.length; ++i) h = (h ^ b.data[i]) * 1099511628211ULL;
        return mix(h);
    }
    return mix(static_cast<uint64_t>(readInteger(row, f)));
}

void writeInt64(uint8_t * row, const Field & f, int64_t v)
{
    std::memcpy(row + f.offset, &v, 8);
    setNull(row, f, false);
}

void writeInt32(uint8_t * row, const Field & f, int32_t v)
{
    std::memcpy(row + f.offset, &v, 4);
    setNull(row, f, false);
}

void writeDouble(uint8_t * row, const Field & f, double v)
{
    std::memcpy(row + f.offset, &v, 8);
    setNull(row, f, false);
}

/// The value of \p in of \p src as \p out of \p dst (of the same type, NULL as NULL)
void copyValue(uint8_t * dst, const Field & out, const uint8_t * src, const Field & in)
{
    const bool null = isNull(src, in);
    if (!null) std::memcpy(dst + out.offset, src + in.offset, std::min(in.size, out.size));
    setNull(dst, out, null);
}

//-----------------------------------------------------------------------------
// aggregates

/// The state of an aggregate of rows: their values that are not NULL, the sum, and the least or greatest row
struct Aggregate {
    __int128 sum = 0;
    double real = 0;
    uint64_t count = 0;
    const uint8_t * best = nullptr;
};

bool isExtreme(Function f)
{
    return f == Function::Min || f == Function::Max;
}

Aggregate leaf(const uint8_t * row, const Call & call)
{
    Aggregate a;
    const Field & value = call.arguments[0];
    if (isNull(row, value)) return a;
    a.count = 1;
    if (isExtreme(call.function)) a.best = row;
    else if (call.function == Function::Count) { }
    else if (value.type.typeID == ID::DoubleID) a.real = readDouble(row, value);
    else a.sum = readInteger(row, value);
    return a;
}

void combine(Aggregate & a, const Aggregate & b, const Call & call)
{
    a.count += b.count;
    a.sum += b.sum;
    a.real += b.real;
    if (b.best != nullptr && (a.best == nullptr || (call.function == Function::Min ? compareValues(b.best, a.best, call.arguments[0]) < 0
                                                                                      : compareValues(b.best, a.best, call.arguments[0]) > 0))) {
        a.best = b.best;
    }
}

/// Writes the result of an aggregate, as the aggregation computes it (NULL without values)
void writeAggregate(uint8_t * row, const Call & call, const Aggregate & a)
{
    const Field & out = call.output;
    if (call.function == Function::Count) {
        writeInt64(row, out, static_cast<int64_t>(a.count));
        return;
    }
    if (a.count == 0) {
        setNull(row, out, true);
        return;
    }
    if (isExtreme(call.function)) {
        copyValue(row, out, a.best, call.arguments[0]);
        return;
    }
    const bool average = call.function == Function::Avg;
    if (call.arguments[0].type.typeID == ID::DoubleID) {
        writeDouble(row, out, average ? a.real / static_cast<double>(a.count) : a.real);
        return;
    }
    const __int128 v = average ? a.sum / static_cast<__int128>(a.count) : a.sum;
    if (out.type.typeID == ID::IntegerID) {
        if (v < INT32_MIN || v > INT32_MAX) throw std::runtime_error("numeric field overflow in an aggregate");
        writeInt32(row, out, static_cast<int32_t>(v));
    } else {
        if (v < INT64_MIN || v > INT64_MAX) throw std::runtime_error("numeric field overflow in an aggregate");
        writeInt64(row, out, static_cast<int64_t>(v));
    }
}

bool isAggregate(Function f)
{
    return f == Function::Count || f == Function::Sum || f == Function::Avg || f == Function::Min || f == Function::Max;
}

bool wholePartition(const Call & c, bool ordered)
{
    if (c.start.kind != BoundKind::UnboundedPreceding) return false;
    if (c.end.kind == BoundKind::UnboundedFollowing) return true;
    // without ORDER BY all rows are peers of the current row
    return !ordered && c.end.kind == BoundKind::CurrentRow && c.mode != Mode::Rows;
}

bool running(const Call & c)
{
    return c.start.kind == BoundKind::UnboundedPreceding && c.end.kind == BoundKind::CurrentRow;
}

bool framed(Function f)
{
    return isAggregate(f) || f == Function::CountAll || f == Function::FirstValue || f == Function::LastValue || f == Function::NthValue;
}

} // namespace

//-----------------------------------------------------------------------------
// a hash group: rows of several partitions, sorted by the partition keys and the order keys

struct Rows::Group {
    const Layout & layout;
    std::vector<uint8_t> data;
    std::vector<uint8_t *> rows;
    // the peer groups (of the order keys within the partitions): the index of each row's, and where each begins
    std::vector<size_t> peerIndex, peerStart;
    // a segment tree per call (empty if the call needs none): the levels above the rows
    std::vector<std::vector<std::vector<Aggregate>>> trees;

    explicit Group(const Layout & l) : layout(l) { }

    size_t size() const { return rows.size(); }

    int comparePartition(const uint8_t * a, const uint8_t * b) const
    {
        for (const Field & f : layout.partition) {
            const bool aNull = isNull(a, f), bNull = isNull(b, f);
            if (aNull || bNull) {
                if (aNull == bNull) continue;
                return aNull ? 1 : -1;
            }
            const int c = compareValues(a, b, f);
            if (c != 0) return c;
        }
        return 0;
    }

    int compareOrder(const uint8_t * a, const uint8_t * b) const
    {
        for (const Key & k : layout.order) {
            const bool aNull = isNull(a, k.field), bNull = isNull(b, k.field);
            if (aNull || bNull) {
                if (aNull == bNull) continue;
                return aNull == k.nullsFirst ? -1 : 1;
            }
            const int c = compareValues(a, b, k.field);
            if (c != 0) return k.descending ? -c : c;
        }
        return 0;
    }

    bool less(const uint8_t * a, const uint8_t * b) const
    {
        const int c = comparePartition(a, b);
        return c != 0 ? c < 0 : compareOrder(a, b) < 0;
    }

    bool peers(size_t i, size_t j) const
    {
        return comparePartition(rows[i], rows[j]) == 0 && compareOrder(rows[i], rows[j]) == 0;
    }

    size_t partitionBegin(size_t pos) const
    {
        if (layout.partition.empty()) return 0;
        const uint8_t * row = rows[pos];
        return std::lower_bound(rows.begin(), rows.begin() + pos, row, [&](const uint8_t * a, const uint8_t * b) {
            return comparePartition(a, b) < 0;
        }) - rows.begin();
    }

    size_t partitionEnd(size_t pos) const
    {
        if (layout.partition.empty()) return rows.size();
        const uint8_t * row = rows[pos];
        return std::upper_bound(rows.begin() + pos, rows.end(), row, [&](const uint8_t * a, const uint8_t * b) {
            return comparePartition(a, b) < 0;
        }) - rows.begin();
    }

    /// The first peer of the row at pos in its partition
    size_t firstPeer(size_t pBegin, size_t pos) const
    {
        if (layout.order.empty()) return pBegin;
        const uint8_t * row = rows[pos];
        return std::lower_bound(rows.begin() + pBegin, rows.begin() + pos, row, [&](const uint8_t * a, const uint8_t * b) {
            return compareOrder(a, b) < 0;
        }) - rows.begin();
    }

    /// After the last peer of the row at pos in its partition
    size_t peersEnd(size_t pos, size_t pEnd) const
    {
        if (layout.order.empty()) return pEnd;
        const uint8_t * row = rows[pos];
        return std::upper_bound(rows.begin() + pos, rows.begin() + pEnd, row, [&](const uint8_t * a, const uint8_t * b) {
            return compareOrder(a, b) < 0;
        }) - rows.begin();
    }

    //-------------------------------------------------------------------------
    // preparation

    /// A row to sort: its keys in 128 bits that compare as the sort order (normalized keys); equal keys compare the rows unless exact
    struct Entry {
        unsigned __int128 key;
        uint8_t * row;
    };

    bool exact = true; // whether the keys hold the whole sort order (no strings, at most 128 bits)

    /// \returns The normalized key: the partition keys, then the order keys, each a NULL bit and its value (descending inverted)
    unsigned __int128 normalize(const uint8_t * row, bool & complete) const
    {
        unsigned __int128 key = 0;
        size_t used = 0;
        complete = true;
        auto put = [&](uint64_t v, unsigned bits) {
            if (!complete) return;
            if (used + bits > 128) {
                // the leading bits only: the key still orders, but not completely
                const unsigned room = static_cast<unsigned>(128 - used);
                if (room > 0) key |= static_cast<unsigned __int128>(v >> (bits - room));
                used = 128;
                complete = false;
                return;
            }
            key |= static_cast<unsigned __int128>(v) << (128 - used - bits);
            used += bits;
        };
        auto encode = [&](const Field & f, bool descending, bool nullsFirst) {
            const bool null = isNull(row, f);
            put(null == nullsFirst ? 0 : 1, 1);
            const ID id = f.type.typeID;
            unsigned bits = 64;
            uint64_t v = 0;
            bool prefix = false;
            if (id == ID::BoolID) {
                bits = 1;
                if (!null) v = readInteger(row, f) != 0;
            } else if (id == ID::IntegerID || id == ID::DateID) {
                bits = 32;
                if (!null) v = static_cast<uint32_t>(static_cast<int32_t>(readInteger(row, f))) ^ 0x80000000u;
            } else if (id == ID::DoubleID) {
                if (!null) {
                    double d = readDouble(row, f);
                    if (d == 0) d = 0;
                    std::memcpy(&v, &d, 8);
                    v = (v >> 63) ? ~v : v | (1ULL << 63);
                }
            } else if (isString(id)) {
                prefix = true;
                if (!null) {
                    const Bytes b = readString(row, f);
                    for (size_t i = 0; i < 8; ++i) v = (v << 8) | (i < b.length ? b.data[i] : 0);
                }
            } else if (!null) {
                v = static_cast<uint64_t>(readInteger(row, f)) ^ (1ULL << 63);
            }
            if (descending && !null) v = bits == 64 ? ~v : (~v & ((1ULL << bits) - 1));
            put(v, bits);
            if (prefix) complete = false;
        };
        for (const Field & f : layout.partition) encode(f, false, false);
        for (const Key & k : layout.order) encode(k.field, k.descending, k.nullsFirst);
        return key;
    }

    bool lessEntry(const Entry & a, const Entry & b) const
    {
        if (a.key != b.key) return a.key < b.key;
        return !exact && less(a.row, b.row);
    }

    std::vector<Entry> entries(size_t workers)
    {
        std::vector<Entry> out(rows.size());
        const size_t chunks = std::max<size_t>(1, std::min(workers * 4, rows.size() / 4096));
        std::vector<uint8_t> complete(chunks, 1);
        Parallel::runTasks(chunks > 1 ? workers : 1, chunks, [&](size_t k, size_t) {
            for (size_t i = rows.size() * k / chunks; i < rows.size() * (k + 1) / chunks; ++i) {
                bool whole;
                out[i] = {normalize(rows[i], whole), rows[i]};
                if (!whole) complete[k] = 0;
            }
        });
        exact = std::all_of(complete.begin(), complete.end(), [](uint8_t c) { return c != 0; });
        return out;
    }

    /// The rows in the order of the entries, copied into one array (the evaluation then reads them in order)
    void materialize(const std::vector<Entry> & sorted, size_t workers)
    {
        const size_t rowSize = layout.rowSize, n = sorted.size();
        std::vector<uint8_t> ordered(n * rowSize);
        const size_t chunks = std::max<size_t>(1, std::min(workers * 4, n / 4096));
        Parallel::runTasks(chunks > 1 ? workers : 1, chunks, [&](size_t k, size_t) {
            for (size_t i = n * k / chunks; i < n * (k + 1) / chunks; ++i) {
                std::memcpy(ordered.data() + i * rowSize, sorted[i].row, rowSize);
                rows[i] = ordered.data() + i * rowSize;
            }
        });
        data.swap(ordered);
    }

    void sort()
    {
        std::vector<Entry> sorted = entries(1);
        std::sort(sorted.begin(), sorted.end(), [&](const Entry & a, const Entry & b) { return lessEntry(a, b); });
        materialize(sorted, 1);
    }

    /// A sample sort: splitters from a sample cut the entries into buckets, which the workers scatter and then sort
    void parallelSort(size_t workers)
    {
        std::vector<Entry> sorted = entries(workers);
        const size_t n = sorted.size(), buckets = workers * 4, chunks = workers * 4;
        auto lessEntries = [&](const Entry & a, const Entry & b) { return lessEntry(a, b); };
        // the splitters: quantiles of a sample of evenly spaced entries
        std::vector<Entry> sample;
        const size_t samples = std::min(n, buckets * 64);
        for (size_t i = 0; i < samples; ++i) sample.push_back(sorted[(n / samples) * i + (n / samples) / 2]);
        std::sort(sample.begin(), sample.end(), lessEntries);
        std::vector<Entry> splitters;
        for (size_t b = 1; b < buckets; ++b) splitters.push_back(sample[samples * b / buckets]);
        // the bucket of each entry, counted per chunk, then scattered to the offsets of the counts
        std::vector<uint32_t> bucketOf(n);
        std::vector<size_t> counts(chunks * buckets, 0);
        Parallel::runTasks(workers, chunks, [&](size_t c, size_t) {
            for (size_t i = n * c / chunks; i < n * (c + 1) / chunks; ++i) {
                const size_t b = std::upper_bound(splitters.begin(), splitters.end(), sorted[i], lessEntries) - splitters.begin();
                bucketOf[i] = static_cast<uint32_t>(b);
                ++counts[c * buckets + b];
            }
        });
        std::vector<size_t> offsets(chunks * buckets), begins(buckets + 1, 0);
        size_t at = 0;
        for (size_t b = 0; b < buckets; ++b) {
            begins[b] = at;
            for (size_t c = 0; c < chunks; ++c) { offsets[c * buckets + b] = at; at += counts[c * buckets + b]; }
        }
        begins[buckets] = n;
        std::vector<Entry> scattered(n);
        Parallel::runTasks(workers, chunks, [&](size_t c, size_t) {
            for (size_t i = n * c / chunks; i < n * (c + 1) / chunks; ++i) scattered[offsets[c * buckets + bucketOf[i]]++] = sorted[i];
        });
        Parallel::runTasks(workers, buckets, [&](size_t b, size_t) {
            std::sort(scattered.begin() + begins[b], scattered.begin() + begins[b + 1], lessEntries);
        });
        materialize(scattered, workers);
    }

    /// The peer groups, counted over ranges of rows that the workers scan, then numbered from the counts
    void computePeers(size_t workers)
    {
        const size_t n = rows.size(), ranges = std::max<size_t>(1, std::min(workers * 4, n / 4096));
        peerIndex.assign(n, 0);
        std::vector<size_t> starts(ranges, 0);
        auto begins = [&](size_t i) { return i == 0 || !peers(i - 1, i); };
        auto range = [&](size_t r) { return std::make_pair(n * r / ranges, n * (r + 1) / ranges); };
        Parallel::runTasks(ranges > 1 ? workers : 1, ranges, [&](size_t r, size_t) {
            auto [b, e] = range(r);
            for (size_t i = b; i < e; ++i) starts[r] += begins(i);
        });
        std::vector<size_t> before(ranges, 0);
        for (size_t r = 1; r < ranges; ++r) before[r] = before[r - 1] + starts[r - 1];
        peerStart.assign(before.back() + starts.back(), 0);
        Parallel::runTasks(ranges > 1 ? workers : 1, ranges, [&](size_t r, size_t) {
            auto [b, e] = range(r);
            size_t index = before[r];
            for (size_t i = b; i < e; ++i) {
                if (begins(i)) peerStart[index++] = i;
                peerIndex[i] = index - 1;
            }
        });
    }

    /// A segment tree for the call: each level holds the aggregates of fanout nodes of the level below, the rows the lowest
    void buildTree(size_t c, size_t workers)
    {
        const Call & call = layout.calls[c];
        auto & levels = trees[c];
        levels.clear();
        size_t below = rows.size();
        while (below > 1) {
            const size_t width = (below + fanout - 1) / fanout;
            std::vector<Aggregate> level(width);
            const std::vector<Aggregate> * lower = levels.empty() ? nullptr : &levels.back();
            const size_t chunks = std::max<size_t>(1, std::min(workers * 4, width / 1024));
            Parallel::runTasks(chunks > 1 ? workers : 1, chunks, [&](size_t k, size_t) {
                for (size_t i = width * k / chunks; i < width * (k + 1) / chunks; ++i) {
                    Aggregate a;
                    for (size_t j = i * fanout; j < std::min(below, (i + 1) * fanout); ++j) {
                        combine(a, lower != nullptr ? (*lower)[j] : leaf(rows[j], call), call);
                    }
                    level[i] = a;
                }
            });
            levels.push_back(std::move(level));
            below = width;
        }
    }

    /// The aggregate of the rows [begin, end): from both bounds up the levels, until they meet (the traversal of the paper)
    Aggregate query(size_t c, size_t begin, size_t end) const
    {
        const Call & call = layout.calls[c];
        const auto & levels = trees[c];
        Aggregate a;
        auto node = [&](size_t level, size_t i) { return level == 0 ? leaf(rows[i], call) : levels[level - 1][i]; };
        for (size_t level = 0; begin < end; ++level) {
            const size_t parentBegin = begin / fanout, parentEnd = end / fanout;
            if (parentBegin == parentEnd || level == levels.size()) {
                for (size_t i = begin; i < end; ++i) combine(a, node(level, i), call);
                return a;
            }
            size_t groupBegin = parentBegin * fanout, first = parentBegin;
            if (begin != groupBegin) {
                for (size_t i = begin; i < groupBegin + fanout; ++i) combine(a, node(level, i), call);
                ++first;
            }
            const size_t groupEnd = parentEnd * fanout;
            for (size_t i = groupEnd; i < end; ++i) combine(a, node(level, i), call);
            begin = first;
            end = parentEnd;
        }
        return a;
    }

    Aggregate linear(size_t c, size_t begin, size_t end) const
    {
        Aggregate a;
        for (size_t i = begin; i < end; ++i) combine(a, leaf(rows[i], layout.calls[c]), layout.calls[c]);
        return a;
    }

    //-------------------------------------------------------------------------
    // frames

    /// The offset of a bound of ROWS or GROUPS
    size_t rowOffset(const uint8_t * row, const Bound & b, bool start) const
    {
        if (isNull(row, b.offset)) throw std::runtime_error(std::string("frame ") + (start ? "starting" : "ending") + " offset must not be null");
        const int64_t n = readInteger(row, b.offset);
        if (n < 0) throw std::runtime_error(std::string("frame ") + (start ? "starting" : "ending") + " offset must not be negative");
        return static_cast<size_t>(n);
    }

    /// RANGE n PRECEDING or FOLLOWING: by the single order key, first the row at or after (start) or after (end) key -+ n
    size_t rangeBound(size_t pos, size_t pBegin, size_t pEnd, const Bound & b, bool start) const
    {
        const Key & key = layout.order.at(0);
        const uint8_t * row = rows[pos];
        if (isNull(row, key.field)) return start ? firstPeer(pBegin, pos) : peersEnd(pos, pEnd);
        if (isNull(row, b.offset)) throw std::runtime_error("invalid preceding or following size in window function");
        // the rows that are not NULL: after the NULLs or before them
        size_t lo = pBegin, hi = pEnd;
        if (key.nullsFirst) {
            lo = std::partition_point(rows.begin() + pBegin, rows.begin() + pEnd, [&](const uint8_t * r) { return isNull(r, key.field); }) - rows.begin();
        } else {
            hi = std::partition_point(rows.begin() + pBegin, rows.begin() + pEnd, [&](const uint8_t * r) { return !isNull(r, key.field); }) - rows.begin();
        }
        // forward in the order: + n ascending, - n descending
        const bool forward = (b.kind == BoundKind::Following) != key.descending;
        const ID keyType = key.field.type.typeID, offsetType = b.offset.type.typeID;
        std::function<int(const uint8_t *)> versus; // the order of a row's key to the target in the sort order
        if (keyType == ID::DoubleID || offsetType == ID::DoubleID) {
            auto value = [](const uint8_t * r, const Field & f) { return f.type.typeID == ID::DoubleID ? readDouble(r, f) : double(readInteger(r, f)) / std::pow(10.0, f.type.typeID == ID::NumericID ? f.type.numericLayout.precision : 0); };
            const double n = value(row, b.offset);
            if (n < 0 || std::isnan(n)) throw std::runtime_error("invalid preceding or following size in window function");
            const double target = value(row, key.field) + (forward ? n : -n);
            versus = [&, target](const uint8_t * r) { const double v = value(r, key.field); const int c = v < target ? -1 : v > target ? 1 : 0; return key.descending ? -c : c; };
        } else {
            const unsigned keyScale = keyType == ID::NumericID ? key.field.type.numericLayout.precision : 0;
            const unsigned offsetScale = offsetType == ID::NumericID ? b.offset.type.numericLayout.precision : 0;
            const unsigned scale = std::max(keyScale, offsetScale);
            auto power = [](unsigned e) { __int128 p = 1; while (e-- > 0) p *= 10; return p; };
            const __int128 keyFactor = power(scale - keyScale), n = readInteger(row, b.offset) * power(scale - offsetScale);
            if (n < 0) throw std::runtime_error("invalid preceding or following size in window function");
            const __int128 target = readInteger(row, key.field) * keyFactor + (forward ? n : -n);
            versus = [&, target, keyFactor](const uint8_t * r) { const __int128 v = readInteger(r, key.field) * keyFactor; const int c = v < target ? -1 : v > target ? 1 : 0; return key.descending ? -c : c; };
        }
        // the first row at or after the target (start), or after it (end)
        return std::partition_point(rows.begin() + lo, rows.begin() + hi, [&](const uint8_t * r) {
            const int c = versus(r);
            return start ? c < 0 : c <= 0;
        }) - rows.begin();
    }

    /// The frame [begin, end) of the row at pos
    std::pair<size_t, size_t> frame(const Call & call, size_t pos, size_t pBegin, size_t pEnd) const
    {
        const uint8_t * row = rows[pos];
        auto bound = [&](const Bound & b, bool start) -> size_t {
            switch (b.kind) {
                case BoundKind::UnboundedPreceding: return pBegin;
                case BoundKind::UnboundedFollowing: return pEnd;
                case BoundKind::CurrentRow:
                    if (call.mode == Mode::Rows) return start ? pos : pos + 1;
                    return start ? firstPeer(pBegin, pos) : peersEnd(pos, pEnd);
                case BoundKind::Preceding:
                case BoundKind::Following: {
                    const bool preceding = b.kind == BoundKind::Preceding;
                    if (call.mode == Mode::Range) return rangeBound(pos, pBegin, pEnd, b, start);
                    const size_t n = rowOffset(row, b, start);
                    if (call.mode == Mode::Rows) {
                        // the row n before or after (the end after it), within the partition
                        if (preceding) return pos - pBegin >= n ? pos - n + (start ? 0 : 1) : pBegin;
                        return pEnd - pos > n ? pos + n + (start ? 0 : 1) : pEnd;
                    }
                    // GROUPS: the peer group n before or after
                    const size_t g = peerIndex[pos], first = peerIndex[pBegin], last = peerIndex[pEnd - 1];
                    if (preceding) {
                        if (g - first < n) return pBegin;
                        return start ? peerStart[g - n] : peerStart[g - n + 1];
                    }
                    if (last - g < n) return pEnd;
                    return start ? peerStart[g + n] : (g + n == last ? pEnd : peerStart[g + n + 1]);
                }
            }
            return pBegin;
        };
        const size_t begin = bound(call.start, true), end = bound(call.end, false);
        return {begin, std::max(begin, end)};
    }

    //-------------------------------------------------------------------------
    // evaluation

    /// The results of a call for the rows [begin, end) (a range of the group, which may begin within a partition)
    void evaluate(size_t c, size_t begin, size_t end)
    {
        const Call & call = layout.calls[c];
        const Field & out = call.output;
        size_t pBegin = partitionBegin(begin), pEnd = partitionEnd(begin);
        const bool ordered = !layout.order.empty();
        const bool whole = framed(call.function) && wholePartition(call, ordered);
        // a running value: the rank of the last row, the aggregate of [pBegin, aggregated) of the partition
        int64_t rank = 0;
        size_t peerEnd = 0, aggregated = pBegin;
        Aggregate state;
        bool stateValid = false;
        for (size_t pos = begin; pos < end; ++pos) {
            if (pos == pEnd) {
                pBegin = pos;
                pEnd = partitionEnd(pos);
                stateValid = false;
            }
            uint8_t * row = rows[pos];
            const size_t pSize = pEnd - pBegin;
            const bool newPeers = pos == begin || pos == pBegin || !peers(pos - 1, pos);
            switch (call.function) {
                case Function::RowNumber:
                    writeInt64(row, out, static_cast<int64_t>(pos - pBegin + 1));
                    break;
                case Function::Rank:
                case Function::PercentRank:
                    if (newPeers) rank = static_cast<int64_t>((pos == begin ? firstPeer(pBegin, pos) : pos) - pBegin + 1);
                    if (call.function == Function::Rank) writeInt64(row, out, rank);
                    else writeDouble(row, out, pSize > 1 ? double(rank - 1) / double(pSize - 1) : 0.0);
                    break;
                case Function::DenseRank:
                    writeInt64(row, out, static_cast<int64_t>(peerIndex[pos] - peerIndex[pBegin] + 1));
                    break;
                case Function::CumeDist:
                    if (newPeers) peerEnd = peersEnd(pos, pEnd);
                    writeDouble(row, out, double(peerEnd - pBegin) / double(pSize));
                    break;
                case Function::Ntile: {
                    const Field & a = call.arguments[0];
                    if (isNull(row, a)) { setNull(row, out, true); break; }
                    const int64_t buckets = readInteger(row, a);
                    if (buckets <= 0) throw std::runtime_error("argument of ntile must be greater than zero");
                    const size_t i = pos - pBegin, n = static_cast<size_t>(buckets), q = pSize / n, r = pSize % n;
                    const size_t bucket = i < r * (q + 1) ? i / (q + 1) + 1 : (i - r * (q + 1)) / q + r + 1;
                    writeInt32(row, out, static_cast<int32_t>(bucket));
                    break;
                }
                case Function::Lag:
                case Function::Lead: {
                    int64_t offset = 1;
                    if (call.arguments.size() > 1) {
                        if (isNull(row, call.arguments[1])) { setNull(row, out, true); break; }
                        offset = readInteger(row, call.arguments[1]);
                    }
                    const int64_t target = static_cast<int64_t>(pos) + (call.function == Function::Lag ? -offset : offset);
                    if (target >= static_cast<int64_t>(pBegin) && target < static_cast<int64_t>(pEnd)) {
                        copyValue(row, out, rows[static_cast<size_t>(target)], call.arguments[0]);
                    } else if (call.arguments.size() > 2) {
                        copyValue(row, out, row, call.arguments[2]);
                    } else {
                        setNull(row, out, true);
                    }
                    break;
                }
                default: {
                    auto [wBegin, wEnd] = whole ? std::make_pair(pBegin, pEnd) : frame(call, pos, pBegin, pEnd);
                    switch (call.function) {
                        case Function::CountAll:
                            writeInt64(row, out, static_cast<int64_t>(wEnd - wBegin));
                            break;
                        case Function::FirstValue:
                        case Function::LastValue:
                        case Function::NthValue: {
                            size_t target = call.function == Function::LastValue ? wEnd - 1 : wBegin;
                            if (call.function == Function::NthValue) {
                                if (isNull(row, call.arguments[1])) { setNull(row, out, true); break; }
                                const int64_t n = readInteger(row, call.arguments[1]);
                                if (n <= 0) throw std::runtime_error("argument of nth_value must be greater than zero");
                                target = wBegin + static_cast<size_t>(n - 1);
                            }
                            if (wBegin < wEnd && target < wEnd) copyValue(row, out, rows[target], call.arguments[0]);
                            else setNull(row, out, true);
                            break;
                        }
                        default: {
                            // an aggregate: once per partition, running, or from the segment tree
                            const bool tree = !trees[c].empty();
                            if (whole) {
                                if (!stateValid) state = tree ? query(c, pBegin, pEnd) : linear(c, pBegin, pEnd);
                                stateValid = true;
                            } else if (running(call) && !tree) {
                                if (!stateValid) { state = Aggregate(); aggregated = pBegin; stateValid = true; }
                                for (; aggregated < wEnd; ++aggregated) combine(state, leaf(rows[aggregated], call), call);
                            } else {
                                state = query(c, wBegin, wEnd);
                            }
                            writeAggregate(row, call, state);
                        }
                    }
                }
            }
        }
    }

    /// The structures the calls need: peer groups and segment trees
    void prepare(size_t workers, bool split)
    {
        const bool ordered = !layout.order.empty();
        bool needPeers = false;
        trees.assign(layout.calls.size(), {});
        for (size_t c = 0; c < layout.calls.size(); ++c) {
            const Call & call = layout.calls[c];
            if (call.function == Function::DenseRank || (framed(call.function) && call.mode == Mode::Groups)) needPeers = true;
            if (isAggregate(call.function) && !wholePartition(call, ordered) && (split || !running(call))) buildTree(c, workers);
            if (isAggregate(call.function) && wholePartition(call, ordered) && split) buildTree(c, workers);
        }
        if (needPeers) computePeers(workers);
    }
};

//-----------------------------------------------------------------------------
// Rows

Rows::Rows(Layout layout, size_t workers) :
        _layout(std::move(layout)),
        _locals(std::max<size_t>(1, workers))
{
    for (auto & local : _locals) local.stage.resize(_layout.rowSize);
}

Rows::~Rows()
{
    reset();
}

void Rows::reset()
{
    for (auto & local : _locals) local.groups.clear();
    for (Group * g : _groups) delete g;
    _groups.clear();
    _order.clear();
}

uint8_t * Rows::stage(size_t worker)
{
    return _locals[worker].stage.data();
}

void Rows::insert(size_t worker)
{
    Local & local = _locals[worker];
    if (local.groups.empty()) local.groups.resize(hashGroups);
    uint64_t h = 0;
    for (const Field & f : _layout.partition) h = mix(h ^ hashValue(local.stage.data(), f));
    auto & group = local.groups[h & (hashGroups - 1)];
    group.insert(group.end(), local.stage.begin(), local.stage.end());
}

void Rows::run()
{
    const size_t rowSize = _layout.rowSize;
    size_t total = 0;
    for (auto & local : _locals) for (auto & g : local.groups) total += g.size() / rowSize;
    if (total == 0) return;
    const size_t workers = total >= parallelRows ? std::max<size_t>(1, Parallel::threads) : 1;

    // combine the hash groups of the workers, each into one array
    std::vector<size_t> nonEmpty;
    for (size_t g = 0; g < hashGroups; ++g) {
        for (auto & local : _locals) if (!local.groups.empty() && !local.groups[g].empty()) { nonEmpty.push_back(g); break; }
    }
    _groups.assign(nonEmpty.size(), nullptr);
    for (size_t i = 0; i < nonEmpty.size(); ++i) _groups[i] = new Group(_layout);
    Parallel::runTasks(workers, nonEmpty.size(), [&](size_t i, size_t) {
        Group & group = *_groups[i];
        size_t bytes = 0;
        for (auto & local : _locals) if (!local.groups.empty()) bytes += local.groups[nonEmpty[i]].size();
        group.data.reserve(bytes);
        for (auto & local : _locals) {
            if (local.groups.empty()) continue;
            auto & part = local.groups[nonEmpty[i]];
            group.data.insert(group.data.end(), part.begin(), part.end());
            std::vector<uint8_t>().swap(part);
        }
        group.rows.resize(bytes / rowSize);
        for (size_t r = 0; r < group.rows.size(); ++r) group.rows[r] = group.data.data() + r * rowSize;
    });

    // inter-partition parallelism for the small hash groups, intra-partition parallelism for each large one
    std::vector<Group *> small, large;
    for (Group * g : _groups) (workers > 1 && g->size() * workers > total && g->size() >= parallelRows ? large : small).push_back(g);
    Parallel::runTasks(workers, small.size(), [&](size_t i, size_t) {
        Group & g = *small[i];
        g.sort();
        g.prepare(1, false);
        for (size_t c = 0; c < _layout.calls.size(); ++c) g.evaluate(c, 0, g.size());
    });
    for (Group * g : large) {
        g->parallelSort(workers);
        g->prepare(workers, true);
        const size_t ranges = workers * 4;
        Parallel::runTasks(workers, ranges * _layout.calls.size(), [&](size_t t, size_t) {
            const size_t c = t / ranges, r = t % ranges;
            g->evaluate(c, g->size() * r / ranges, g->size() * (r + 1) / ranges);
        });
    }

    _order.reserve(total);
    for (Group * g : _groups) _order.insert(_order.end(), g->rows.begin(), g->rows.end());
}

} // end namespace WindowEvaluation
} // end namespace Physical
} // end namespace Algebra
