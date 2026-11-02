#pragma once

#include <tuple>

#include "foundations/Database.hpp"
#include "queryCompiler/QueryContext.hpp"
#include "native/sql/Register.hpp"
#include "native/sql/SqlValues.hpp"
#include "native/sql/SqlTuple.hpp"
#include "utils/optimistic_lock.hpp"

#include <boost/dynamic_bitset.hpp>

class Table;

struct VersionedTupleStorage {
    const void * next = nullptr;
    const void * next_in_branch = nullptr;
    branch_id_t branch_id;
    branch_id_t creation_ts; // latest branch id during the time of creation
    ident_t ident; // identifier of the branch at the time of creation
    uint8_t data[0];
};

// similar to VersionedTupleStorage; used by the current 'master' branch entry
struct VersionEntry {
    void * first = nullptr;
    void * prev = nullptr;
    void * next = nullptr;
    VersionedTupleStorage * next_in_branch = nullptr;
    branch_id_t branch_id;
    branch_id_t creation_ts; // latest branch id during the time of creation (same as the length of the branch bitvector)
    ident_t ident; // identifier of the branch at the time of creation
    opt_lock::lock_t lock;
    boost::dynamic_bitset<> branch_visibility;
};

inline tid_t mark_as_dangling_tid(tid_t tid) {
    return (tid | static_cast<decltype(tid)>(1) << (8*sizeof(decltype(tid))-1));
}

inline tid_t unmark_dangling_tid(tid_t tid) {
    return (tid & ~(static_cast<decltype(tid)>(1) << (8*sizeof(decltype(tid))-1)));
}

inline bool is_marked_as_dangling_tid(tid_t tid) {
    return (static_cast<decltype(tid)>(0) != (tid & static_cast<decltype(tid)>(1) << (8*sizeof(decltype(tid))-1)));
}

inline bool has_lineage_intersection(QueryContext & ctx, VersionEntry * version_entry) {
    return (ctx.executionContext.branch_lineage_bitset.intersects(version_entry->branch_visibility));
}

VersionEntry * get_version_entry(tid_t tid, Table & table);

const void * get_latest_chain_element(const VersionEntry * version_entry, Table & table, QueryContext & ctx);

const void * get_earliest_chain_element(const VersionEntry * version_entry, Table & table, QueryContext & ctx);

const void * get_chain_element(const VersionEntry * version_entry, unsigned revision_offset, Table & table, QueryContext & ctx);

//branch_id_t create_branch(std::string name, branch_id_t parent);

// FIXME tuple has to exist in the master branch!
tid_t insert_tuple(Native::Sql::SqlTuple & tuple, Table & table, QueryContext & ctx);
tid_t insert_tuple_with_branchId(Native::Sql::SqlTuple & tuple, Table & table, QueryContext & ctx, branch_id_t branchId);

void update_tuple(tid_t tid, Native::Sql::SqlTuple & tuple, Table & table, QueryContext & ctx);
void update_tuple_with_branchId(tid_t tid, branch_id_t branchId, Native::Sql::SqlTuple & tuple, Table & table, QueryContext & ctx);

// the entire version chain has to be deleted
// the tuple has to be relocated iff branch==master
void delete_tuple(tid_t tid, Table & table, QueryContext & ctx);
void delete_tuple_with_branchId(tid_t tid, branch_id_t branchId, Table & table, QueryContext & ctx);

tid_t merge_tuple(branch_id_t src_branch, branch_id_t dst_branch, tid_t tid, QueryContext ctx);

std::unique_ptr<Native::Sql::SqlTuple> get_latest_tuple(tid_t tid, Table & table, QueryContext & ctx);

/// Returned by get_latest_entry() and get_latest_entry_of_reader() if the branch sees no version of
/// the tuple. With lazy membership, a branch also contains the tuples its parent inserted after the
/// fork, whose versions it does not see; with eager membership, this is an error.
inline const void * const invisible_version = reinterpret_cast<const void *>(uintptr_t(1));

const void *get_latest_entry(tid_t tid, Table & table, branch_id_t branchId, QueryContext & ctx);

/// As get_latest_entry, but leaves the context unchanged, so that threads can share it. The reader is
/// given by its label (interval visibility) or its lineage map (otherwise).
const void * get_latest_entry_of_reader(tid_t tid, Table & table, branch_id_t branchId, uint64_t reader_label,
                                        const std::unordered_map<branch_id_t, branch_id_t> * lineage);

/// The branches read by a cross-branch query. Readers are kept in the order of their labels, so
/// that the readers seeing a version form one range of positions.
struct BranchSet {
    std::vector<branch_id_t> branches; // reader at position p
    std::vector<uint64_t> labels;      // label of the reader at position p, ascending
};

/// Splits the readers of a branch set by the version of tuple tid each of them sees, walking the
/// version chain once. Part i is seen by the readers at the positions lo[i]..hi[i]; ptrs[i] is
/// nullptr for the in-place version and the tuple data otherwise. Readers whose bitmap does not
/// contain the tuple are left out; all_present states that all of them contain it.
/// The output arrays hold one entry per reader. Returns the number of parts.
uint32_t get_visible_ranges(tid_t tid, Table & table, const BranchSet & set, bool all_present,
                            const void ** ptrs, uint32_t * lo, uint32_t * hi);

bool is_visible(tid_t tid, Table & table, QueryContext & ctx);

/// Adds the tuple, written by the branch, to the indexes of the branch
void index_tuple(Table & table, branch_id_t branch, tid_t tid, const Native::Sql::SqlTuple & tuple);

/// Fills an index from the versions visible in its branch
void build_index(Table & table, BranchIndex & index);

/// A reader of one branch, independent of a query context
class BranchReader {
public:
    BranchReader(const Database & db, branch_id_t branch);

    /// \returns Whether the branch sees a version written by the given branch, at the given time, under the given identifier
    bool sees(branch_id_t creator, branch_id_t creation_ts, ident_t ident) const;

    /// \returns The newest version of the tuple visible in the branch: the version entry for the
    /// in-place version, a VersionedTupleStorage otherwise, or nullptr; the bitmap is not consulted
    const void * visible(const VersionEntry * entry) const;

    /// \returns Whether the branch sees the chain element (the version entry or a VersionedTupleStorage)
    bool sees(const VersionEntry * entry, const void * element) const;

    const branch_id_t branch;

private:
    const Database & _db;
    uint64_t _label = 0;
    std::unordered_map<branch_id_t, branch_id_t> _lineage; // ancestor -> its child on the path to the branch
};

/// \returns The tuple of a chain element of tid (see BranchReader::visible)
std::unique_ptr<Native::Sql::SqlTuple> load_version(tid_t tid, Table & table, const void * element);

/// The outcome of MERGE BRANCH src INTO dst
struct MergeResult {
    size_t updated = 0;
    size_t inserted = 0;
    size_t deleted = 0;
    std::vector<std::string> conflicts; // nothing has been merged if there are any
};

/// Merges the changes of src since its common versions with dst into dst. Per tuple, the base is
/// the newest version visible in both branches, or the version of src merged last into dst.
/// Attributes changed on one side only are taken from that side; an attribute changed differently
/// on both sides, or a tuple deleted on one side and changed on the other, is a conflict. The merge
/// is applied only if there are no conflicts.
MergeResult merge_branch(Database & db, branch_id_t src, branch_id_t dst);

/// Drops the branches (a branch with all its descendants): clears their bitmaps, frees the versions
/// they wrote and removes them from the catalogue. \returns The number of freed versions
size_t drop_branches(Database & db, const std::vector<branch_id_t> & branches);

/// \returns The tuples in which the branches may differ, ascending: those written under an identifier
/// that is an ancestor of one of the branches only, i.e. after the fork at which their paths split
std::vector<tid_t> diverging_tuples(Table & table, branch_id_t a, branch_id_t b);

/// DIFF and MERGE scan the whole table instead of the diverging tuples (for comparisons)
extern bool diff_scans_whole_table;

/// One side of a difference: the version of tuple tid seen in one of two branches
struct DiffEntry {
    tid_t tid;
    const void * data; // nullptr for the in-place version, the tuple data of a VersionedTupleStorage otherwise
    uint32_t side;     // 0: first branch, 1: second branch
};

/// Appends the tuples that differ between two branches: a tuple only in one branch yields one
/// entry, a tuple whose versions differ in value yields one entry per branch
void diff_branches(Table & table, branch_id_t a, branch_id_t b, std::vector<DiffEntry> & out);

void destroy_chain(VersionEntry * version_entry);

std::unique_ptr<Native::Sql::SqlTuple> get_current_master(tid_t tid, Table & table);

template<typename RegisterType>
struct ScanItem {
    const Vector & column;
    size_t offset;
    RegisterType reg;

    ScanItem(const Vector & column, size_t offset)
        : column(column)
        , offset(offset)
    { }

    ScanItem(const ScanItem &) = delete;

    // https://stackoverflow.com/a/15730993
    ScanItem(ScanItem && other) noexcept
        : column(other.column)
        , offset(other.offset)
        , reg() // FIXME preserve value
    { }
};

template<typename Consumer, typename... Ts>
inline void produce_current_master(tid_t tid, Consumer consumer, std::tuple<Ts...> & scan_items) {
    std::apply([&tid] (auto &... item) {
        (item.reg.load_from(item.column.at(tid)), ...);
    }, scan_items);
    consumer(scan_items);
}

template<typename Consumer, typename... Ts>
inline void produce(const VersionedTupleStorage * storage, Consumer consumer, std::tuple<Ts...> & scan_items) {
    const uint8_t * tuple_ptr = storage->data;
    std::apply([tuple_ptr] (auto &... item) {
        (item.reg.load_from(tuple_ptr + item.offset), ...);
    }, scan_items);
    consumer(scan_items);
}

template<typename Consumer, typename... Ts>
void produce_latest_tuple(QueryContext & ctx, tid_t tid, Table & table, Consumer consumer, std::tuple<Ts...> & scan_items) {
    branch_id_t branch = ctx.executionContext.branchId;
    if (branch == master_branch_id) {
        produce_current_master(tid, consumer, scan_items);
        return;
    }

    const auto version_entry = get_version_entry(tid, table);
    if (!has_lineage_intersection(ctx, version_entry)) {
        return;
    }
    const void * element = get_latest_chain_element(version_entry, table, ctx);

    if (element == nullptr) {
        return;
    } else if (element == version_entry) { // current master
        produce_current_master(tid, consumer, scan_items);
    } else {
        auto storage = static_cast<const VersionedTupleStorage *>(element);
        produce(storage, consumer, scan_items);
    }
}


template<typename Consumer, typename... Ts>
void produce_earliest_tuple(QueryContext & ctx, tid_t tid, Table & table, Consumer consumer, std::tuple<Ts...> & scan_items) {
    const auto version_entry = get_version_entry(tid, table);
    if (!has_lineage_intersection(ctx, version_entry)) {
        return;
    }
    const void * element = get_earliest_chain_element(version_entry, table, ctx);

    if (element == nullptr) {
        return;
    } else if (element == version_entry) { // current master
        produce_current_master(tid, consumer, scan_items);
    } else {
        auto storage = static_cast<const VersionedTupleStorage *>(element);
        produce(storage, consumer, scan_items);
    }
}


// revision_offset == 0 => latest revision
std::unique_ptr<Native::Sql::SqlTuple> get_tuple(tid_t tid, unsigned revision_offset, Table & table, QueryContext & ctx);

template<typename Consumer, typename... Ts>
void produce_tuple(QueryContext & ctx, tid_t tid, unsigned revision_offset, Table & table, Consumer consumer, std::tuple<Ts...> & scan_items) {
    const auto version_entry = get_version_entry(tid, table);
    if (!has_lineage_intersection(ctx, version_entry)) {
        return;
    }
    const void * element = get_chain_element(version_entry, revision_offset, table, ctx);

    if (element == nullptr) {
        return;
    } else if (element == version_entry) { // current master
        produce_current_master(tid, consumer, scan_items);
    } else {
        auto storage = static_cast<const VersionedTupleStorage *>(element);
        produce(storage, consumer, scan_items);
    }
}

template<typename Consumer, typename... Ts>
void scan_relation_yielding_earliest(QueryContext & ctx, Table & table, Consumer consumer, std::tuple<Ts...> & scan_items) {
    branch_id_t branch = ctx.executionContext.branchId;
    if (branch == master_branch_id) {
        auto size = table._version_mgmt_column.size();
        for (tid_t tid = 0; tid < size; ++tid) {
            produce_earliest_tuple(ctx, tid, table, consumer, scan_items);
        }
    } else {
        auto size = table._version_mgmt_column.size();
        for (tid_t tid = 0; tid < size; ++tid) {
            produce_earliest_tuple(ctx, tid, table, consumer, scan_items);
        }
        size = table._dangling_version_mgmt_column.size();
        for (tid_t tid = 0; tid < size; ++tid) {
            tid_t marked = mark_as_dangling_tid(tid);
            produce_earliest_tuple(ctx, marked, table, consumer, scan_items);
        }
    }
}

template<typename Consumer, typename... Ts>
void scan_relation_yielding_latest(QueryContext & ctx, Table & table, Consumer consumer, std::tuple<Ts...> & scan_items) {
    branch_id_t branch = ctx.executionContext.branchId;
    if (branch == master_branch_id) {
        auto size = table._version_mgmt_column.size();
        for (tid_t tid = 0; tid < size; ++tid) {
            produce_current_master(tid, consumer, scan_items);
        }
    } else {
        auto size = table._version_mgmt_column.size();
        for (tid_t tid = 0; tid < size; ++tid) {
            produce_latest_tuple(ctx, tid, table, consumer, scan_items);
        }
        size = table._dangling_version_mgmt_column.size();
        for (tid_t tid = 0; tid < size; ++tid) {
            tid_t marked = mark_as_dangling_tid(tid);
            produce_latest_tuple(ctx, marked, table, consumer, scan_items);
        }
    }
}
