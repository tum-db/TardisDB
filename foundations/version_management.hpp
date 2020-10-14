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

void update_tuple(tid_t tid, Native::Sql::SqlTuple & tuple, Table & table, QueryContext & ctx);

void update_tuple_with_binding(tid_t tid, std::string *binding, Native::Sql::SqlTuple & tuple, Table & table, QueryContext & ctx);

// the entire version chain has to be deleted
// the tuple has to be relocated iff branch==master
void delete_tuple(tid_t tid, Table & table, QueryContext & ctx);

tid_t merge_tuple(branch_id_t src_branch, branch_id_t dst_branch, tid_t tid, QueryContext ctx);

std::unique_ptr<Native::Sql::SqlTuple> get_latest_tuple(tid_t tid, Table & table, QueryContext & ctx);
const void *get_latest_entry(tid_t tid, Table & table, std::string *binding, QueryContext & ctx);
std::unique_ptr<Native::Sql::SqlTuple> get_latest_tuple_with_binding(std::string *binding, tid_t tid, Table & table, QueryContext & ctx);

bool is_visible(tid_t tid, Table & table, QueryContext & ctx);
uint8_t is_visible_with_binding(tid_t tid, std::string *binding, Table & table, QueryContext & ctx);

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
