#include "foundations/version_management.hpp"

#include <stdexcept>
#include <atomic>
#include <malloc.h>
#include <mutex>

#include <algorithm>
#include <numeric>
#include <iostream>
#include <unordered_set>

#include "codegen/CodeGen.hpp"
#include "codegen/LLVMCompat.hpp"
#include "foundations/exceptions.hpp"
#include "sql/SqlTuple.hpp"
#include "utils/general.hpp"

template<typename T>
static bool is_visible(const T & elem, QueryContext & ctx) {
#if INTERVAL_VISIBILITY
    // visible iff the version's identifier is an ancestor of the reader's identifier (or the same)
    const auto & ec = ctx.executionContext;
    return ec.ident_l[elem.ident] <= ec.reader_label && ec.reader_label <= ec.ident_r[elem.ident];
#endif
    branch_id_t current_branch = ctx.executionContext.branchId;
    if (elem.branch_id == current_branch) {
        return true;
    }
    const auto & branch_lineage = ctx.executionContext.branch_lineage;
    auto it = branch_lineage.find(elem.branch_id);
    if (it != branch_lineage.end() && elem.creation_ts < it->second) {
        return true;
    }
    return false;
}

static std::atomic<size_t> version_count(0);
static std::atomic<size_t> version_bytes(0); // as the allocator gave them (malloc_usable_size)

size_t live_version_bytes() {
    return version_bytes;
}

size_t live_versions() {
    return version_count;
}

bool collect_garbage = true;
bool overwrite_own_versions = true;
size_t version_map_after_scans = 0;

static VersionedTupleStorage * create_chain_element(Table & table, size_t tuple_size) {
    size_t size = sizeof(VersionedTupleStorage) + tuple_size;
    void * mem = std::malloc(size);
    VersionedTupleStorage * storage = new (mem) VersionedTupleStorage();
    // every version holds all columns of the table (complete_tuple())
    storage->columns = table.getColumnCount();
    ++version_count;
    version_bytes += malloc_usable_size(mem);
    return storage;
}

static void free_chain_element(const void * element) {
    version_bytes -= malloc_usable_size(const_cast<void *>(element));
    std::free(const_cast<void *>(element));
    --version_count;
}

inline void * get_tuple_ptr(VersionedTupleStorage * storage) {
    return storage->data;
}

inline const void * get_tuple_ptr(const VersionedTupleStorage * storage) {
    return storage->data;
}

bool diff_scans_whole_table = false;

// remembers that the branch wrote the tuple under its current identifier
/// Sets what the compiled scans read of the head of the chain of tid: the identifier (and with
/// COMPILED_LINEAGE the stamp) of the in-place version if it heads the chain, otherwise invalid.
template<int order = __ATOMIC_RELAXED>
static inline void store_head(Table & table, tid_t tid, const VersionEntry * head) {
    __atomic_store_n(&table._head_ident[tid], head != nullptr ? head->ident : invalid_ident, order);
#if COMPILED_LINEAGE
    __atomic_store_n(&table._head_stamp[tid], head != nullptr ? lineage_stamp(head->branch_id, head->creation_ts) : invalid_stamp, order);
#endif
}

static void record_write(Table & table, branch_id_t branch, tid_t tid, bool deleted = false) {
    if (is_marked_as_dangling_tid(tid)) {
        return;
    }
    const ident_t ident = table.getDatabase().getIdent(branch);
    // writers of other branches record theirs meanwhile, in the lists of their identifiers: growing the
    // outer vectors here would move those lists under them (Database::newIdent() sizes them in advance)
    if (ident >= table._writes.size() || ident >= table._deleted.size()) {
        throw std::logic_error("the write lists of identifier " + std::to_string(ident) + " do not exist");
    }
    auto & tids = table._writes[ident];
    if (tids.empty() || tids.back() != tid) {
        tids.push_back(tid);
    }
    if (deleted) {
        table._deleted[ident].insert(tid);
    } else {
        table._deleted[ident].erase(tid);
    }
}

void index_tuple(Table & table, branch_id_t branch, tid_t tid, const Native::Sql::SqlTuple & tuple) {
    for (auto & index : table._indexes) {
        if (index->branch == branch) {
            index->add(tuple.values[index->column]->hash(), tid);
        }
    }
}

tid_t insert_tuple(Native::Sql::SqlTuple & tuple, Table & table, QueryContext & ctx) {
    tid_t tid;
    branch_id_t branch = ctx.executionContext.branchId;
    Database & db = table.getDatabase();
    size_t branch_cnt = 1 + db.getLargestBranchId();

    tid = table._version_mgmt_column.size();
    // grows only if the statement holds the table exclusive; otherwise it made sure there is room
    table.ensureCapacity(tid + 1);

    auto entry = std::make_unique<VersionEntry>();
    VersionEntry * version_entry = entry.get();
    version_entry->first = version_entry;
    version_entry->next = nullptr;
    version_entry->next_in_branch = nullptr;

    // branch visibility
    version_entry->branch_id = branch;
    version_entry->branch_visibility.resize(branch_cnt, false);
    version_entry->branch_visibility.set(branch);
    version_entry->creation_ts = db.getLargestBranchId();
    version_entry->ident = db.getIdent(branch);
    {
        // the arrays do not move (capacity), but scans compiled meanwhile compare their sizes and add version maps
        std::lock_guard<std::mutex> lock(table._scanLatch);
        table._version_mgmt_column.push_back(std::move(entry));
        table._head_ident.push_back(version_entry->ident);
#if COMPILED_LINEAGE
        table._head_stamp.push_back(lineage_stamp(version_entry->branch_id, version_entry->creation_ts));
#endif
        // only the inserting branch sees the in-place version, since its identifier has no descendants
        for (auto & [reader, versions] : table._versionMaps) {
            versions.push_back(reader == branch ? 0 : 1);
        }
    }

    // store tuple
    table.appendRow(branch);
    size_t column_idx = 0;
    for (auto & value : tuple.values) {
        void * ptr = const_cast<void *>(table.getColumn(column_idx).back());
        value->store(ptr);
        column_idx += 1;
    }
    index_tuple(table, branch, tid, tuple);
    record_write(table, branch, tid);
    // statements that start from now on scan the tuple
    table.publishRows();

    return tid;
}

tid_t insert_tuple_with_branchId(Native::Sql::SqlTuple & tuple, Table & table, QueryContext & ctx, branch_id_t branchId) {
    ctx.executionContext.branchId = branchId;
    return insert_tuple(tuple,table,ctx);
}

VersionEntry * get_version_entry(tid_t tid, Table & table) {
    if (is_marked_as_dangling_tid(tid)) {
        tid_t unmarked = unmark_dangling_tid(tid);
        return table._dangling_version_mgmt_column[unmarked].get();
    } else {
        return table._version_mgmt_column[tid].get();
    }
}

std::unique_ptr<Native::Sql::SqlTuple> get_current_master(tid_t tid, Table & table) {
    std::vector<Native::Sql::value_op_t> values;
    auto tuple_type = table.getTupleType();
    for (size_t i = 0; i < tuple_type.size(); ++i) {
        const void * ptr = table.getColumn(i).at(tid);
        values.push_back(Native::Sql::Value::load(ptr, tuple_type[i]));
    }
    return std::make_unique<Native::Sql::SqlTuple>(std::move(values));
}

static void update_master(tid_t tid, Native::Sql::SqlTuple & tuple, Table & table) {
    size_t column_idx = 0;
    for (auto & value : tuple.values) {
        void * ptr = const_cast<void *>(table.getColumn(column_idx).at(tid));
        value->store(ptr);
        column_idx += 1;
    }
}

// replaces the in-place version in the chain by a copy, which the readers of the old version read from
// now on, while readers of other branches may walk the chain and writers of other branches prepend to it
static void defer_in_place_write(tid_t tid, Native::Sql::SqlTuple & tuple, Table & table, InPlaceWrites & pending) {
    auto version_entry = get_version_entry(tid, table);
    auto found = pending.positions.find(tid);
    if (found != pending.positions.end()) {
        // written before by this statement: the copy is in the chain already
        tuple.store(pending.tuples.data() + found->second * pending.tupleSize);
        return;
    }

    auto old_tuple = get_current_master(tid, table);
    auto storage = create_chain_element(table, old_tuple->getSize());
    storage->branch_id = version_entry->branch_id;
    storage->creation_ts = version_entry->creation_ts;
    storage->ident = version_entry->ident;
    storage->next = version_entry->next;
    storage->next_in_branch = version_entry->next_in_branch;
    old_tuple->store(get_tuple_ptr(storage));

    // only main changes the element after the in-place version, and other writers only prepend
    for (;;) {
        void * head = __atomic_load_n(&version_entry->first, __ATOMIC_ACQUIRE);
        if (head == version_entry) {
            if (__atomic_compare_exchange_n(&version_entry->first, &head, static_cast<void *>(storage), false,
                                            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) break;
            continue; // another branch has prepended a version
        }
        // prev may not be set yet by the writer that has just prepended to the version entry
        auto before = static_cast<VersionedTupleStorage *>(head);
        while (chain_next(version_entry, before) != version_entry) {
            before = const_cast<VersionedTupleStorage *>(static_cast<const VersionedTupleStorage *>(chain_next(version_entry, before)));
        }
        __atomic_store_n(&before->next, static_cast<const void *>(storage), __ATOMIC_RELEASE);
        break;
    }
    store_head(table, tid, nullptr);

    std::lock_guard<std::mutex> lock(table._scanLatch);
    if (pending.tupleSize == 0) pending.tupleSize = tuple.getSize();
    pending.positions.emplace(tid, pending.writes.size());
    pending.writes.push_back({tid, storage});
    pending.tuples.resize(pending.writes.size() * pending.tupleSize);
    tuple.store(pending.tuples.data() + (pending.writes.size() - 1) * pending.tupleSize);
    // the branches that read the old version in place read it from its copy now
    const auto copy = reinterpret_cast<uintptr_t>(get_tuple_ptr(storage));
    for (auto & [reader, versions] : table._versionMaps) {
        if (reader != master_branch_id && versions[tid] == 0) {
            __atomic_store_n(&versions[tid], copy, __ATOMIC_RELAXED);
        }
    }
}

void finish_in_place_writes(Table & table, InPlaceWrites & pending) {
    if (!pending.writes.empty()) {
        // afterwards, no statement reads an old version in place: those that entered later found the copies
        table._readers.synchronize();
        Database & db = table.getDatabase();
        const ident_t ident = db.getIdent(master_branch_id);
        const branch_id_t creation_ts = db.getLargestBranchId();
        const auto types = table.getTupleType();
        for (size_t i = 0; i < pending.writes.size(); ++i) {
            const auto [tid, copy] = pending.writes[i];
            auto version_entry = get_version_entry(tid, table);
            auto tuple = Native::Sql::SqlTuple::load(pending.tuples.data() + i * pending.tupleSize, types);
            update_master(tid, *tuple, table);
            version_entry->next_in_branch = copy;
            version_entry->branch_id = master_branch_id;
            version_entry->creation_ts = creation_ts;
            version_entry->ident = ident;
            // no other branch sees the new version, so it may become the head before or after other
            // branches prepend theirs
            void * head = __atomic_load_n(&version_entry->first, __ATOMIC_ACQUIRE);
            do {
                version_entry->next = head;
            } while (!__atomic_compare_exchange_n(&version_entry->first, &head, static_cast<void *>(version_entry), false,
                                                  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
            store_head<__ATOMIC_SEQ_CST>(table, tid, version_entry);
            if (__atomic_load_n(&version_entry->first, __ATOMIC_SEQ_CST) != version_entry) {
                store_head<__ATOMIC_SEQ_CST>(table, tid, nullptr);
            }
        }
    }
    std::lock_guard<std::mutex> lock(table._scanLatch);
    table._inPlaceWrites = nullptr;
}

// master updates in place; the previous in-place version moves into the chain
static void write_in_place(tid_t tid, Native::Sql::SqlTuple & tuple, Table & table) {
    const branch_id_t branch = master_branch_id;
    Database & db = table.getDatabase();
    auto version_entry = get_version_entry(tid, table);
    if (overwrite_own_versions && version_entry->ident == db.getIdent(branch)) {
        // written under the current identifier, which has no descendants: no other branch sees it
        update_master(tid, tuple, table);
    } else if (table._inPlaceWrites != nullptr) {
        defer_in_place_write(tid, tuple, table, *table._inPlaceWrites);
    } else {
        auto old_tuple = get_current_master(tid, table);

        auto storage = create_chain_element(table, old_tuple->getSize());

        // branch visibility
        storage->branch_id = version_entry->branch_id;
        storage->creation_ts = version_entry->creation_ts;
        storage->ident = version_entry->ident;

        // Hand over next and next_in_branch values from version_entry to storage
        storage->next = version_entry->next;
        storage->next_in_branch = version_entry->next_in_branch;
        // If the head of the chain does not equal the version_entry,
        // adjust the next pointer of the previous storage element to the created one
        if (version_entry->first != version_entry) {
            ((VersionedTupleStorage*)version_entry->prev)->next = storage;
        }

        old_tuple->store(get_tuple_ptr(storage));
        // the branches that read the old version in place read it from its copy now
        for (auto & [reader, versions] : table._versionMaps) {
            if (reader != branch && versions[tid] == 0) {
                versions[tid] = reinterpret_cast<uintptr_t>(get_tuple_ptr(storage));
            }
        }

        // version entry update
        if (version_entry->first != version_entry) {            // next should point to the old head of the chain
            version_entry->next = version_entry->first;
        } else {
            version_entry->next = storage;
        }
        version_entry->next_in_branch = storage;                // next_in_branch points to the inserted storage entry
        version_entry->first = version_entry;                   // the head now points again to the version_entry
        version_entry->branch_id = branch;
        version_entry->creation_ts = db.getLargestBranchId();
        version_entry->ident = db.getIdent(branch);
        store_head(table, tid, version_entry);

        // new master
        update_master(tid, tuple, table);
    }
    index_tuple(table, branch, tid, tuple);
    record_write(table, branch, tid);
}

// other branches prepend their version to the chain; predecessor is the version the branch saw before
static void prepend_version(tid_t tid, branch_id_t branch, Native::Sql::SqlTuple & tuple, Table & table, const void * predecessor) {
    Database & db = table.getDatabase();
    auto version_entry = get_version_entry(tid, table);
    const ident_t ident = db.getIdent(branch);
    if (overwrite_own_versions && predecessor == version_entry && version_entry->ident == ident) {
        // the branch inserted the tuple under its current identifier, which no other branch sees
        update_master(tid, tuple, table);
    } else if (overwrite_own_versions && predecessor != nullptr && predecessor != version_entry &&
               static_cast<const VersionedTupleStorage *>(predecessor)->ident == ident &&
               static_cast<const VersionedTupleStorage *>(predecessor)->columns == table.getColumnCount()) {
        // (a version written before a column was added is too small)
        tuple.store(get_tuple_ptr(const_cast<VersionedTupleStorage *>(static_cast<const VersionedTupleStorage *>(predecessor))));
    } else {
        auto storage = create_chain_element(table, tuple.getSize());

        // branch visibility (branch_visibility is not maintained: no visibility test reads it, and
        // writers of several branches would change it at the same time)
        storage->branch_id = branch;
        storage->creation_ts = db.getLargestBranchId();
        storage->ident = db.getIdent(branch);

        tuple.store(get_tuple_ptr(storage));
        if (!is_marked_as_dangling_tid(tid)) {
            std::lock_guard<std::mutex> lock(table._scanLatch);
            auto versionMap = table._versionMaps.find(branch);
            if (versionMap != table._versionMaps.end()) {
                versionMap->second[tid] = reinterpret_cast<uintptr_t>(get_tuple_ptr(storage));
            }
        }
        storage->next_in_branch = predecessor;

        // Writers of other branches may prepend to the same chain meanwhile; none of them sees this
        // version, nor does any reader of another branch, so publishing it needs no latch: the version
        // is complete before it becomes the head of the chain
        void * head = __atomic_load_n(&version_entry->first, __ATOMIC_ACQUIRE);
        do {
            storage->next = head;
        } while (!__atomic_compare_exchange_n(&version_entry->first, &head, static_cast<void *>(storage), false,
                                              __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
        // prev, the element before the version entry, is read only under the exclusive latch of the table
        if (head == version_entry) version_entry->prev = storage;
        if (!is_marked_as_dangling_tid(tid)) {
            store_head(table, tid, nullptr);
        }
    }
    index_tuple(table, branch, tid, tuple);
    record_write(table, branch, tid);
}

void update_tuple(tid_t tid, Native::Sql::SqlTuple & tuple, Table & table, QueryContext & ctx) {
    branch_id_t branch = ctx.executionContext.branchId;

    if (is_marked_as_dangling_tid(tid) && branch == master_branch_id) {
        throw std::runtime_error("no such tuple in the given branch");
    }

    if (branch == master_branch_id) {
        write_in_place(tid, tuple, table);
    } else {
        auto version_entry = get_version_entry(tid, table);
        auto predecessor = get_latest_chain_element(version_entry, table, ctx);
        if (predecessor == nullptr) {
            throw std::runtime_error("no such tuple in the given branch");
        }
        prepend_version(tid, branch, tuple, table, predecessor);
    }
}

void update_tuple_with_branchId(tid_t tid, branch_id_t branchId, Native::Sql::SqlTuple & tuple, Table & table, QueryContext & ctx) {
    ctx.executionContext.branchId = branchId;
    return update_tuple(tid,tuple,table,ctx);
}

void delete_tuple(tid_t tid, Table & table, QueryContext & ctx) {
    branch_id_t branch = ctx.executionContext.branchId;

    table.removeRowForBranch(tid,branch);
    record_write(table, branch, tid, true);
}

void delete_tuple_with_branchId(tid_t tid, branch_id_t branchId, Table & table, QueryContext & ctx) {
    ctx.executionContext.branchId = branchId;

    delete_tuple(tid,table,ctx);
}

const void * get_latest_chain_element(const VersionEntry * version_entry, Table & table, QueryContext & ctx) {
    branch_id_t branch = ctx.executionContext.branchId;

    if (branch == master_branch_id) {
        return version_entry;
    }

    const void * next = chain_head(version_entry);
    while (next != nullptr) {
        if (next == version_entry) {
            // this is the current master branch
            if (is_visible(*version_entry, ctx)) {
                return version_entry;
            }
        } else {
            const auto storage = static_cast<const VersionedTupleStorage *>(next);
            if (is_visible(*storage, ctx)) {
                return storage;
            }
        }
        next = chain_next(version_entry, next);
    }
    return nullptr;
}

const void * get_earliest_chain_element(const VersionEntry * version_entry, Table & table, QueryContext & ctx) {
    const void * latest = get_latest_chain_element(version_entry, table, ctx);

    auto current = latest;
    auto next = current;
    for (;;) {
        if (next == nullptr) {
            return current;
        }

        current = next;
        if (current == version_entry) {
            next = version_entry->next_in_branch;
        } else {
            next = static_cast<const VersionedTupleStorage *>(current)->next_in_branch;
        }
    }
    return current;
}

std::unique_ptr<Native::Sql::SqlTuple> get_latest_tuple(tid_t tid, Table & table, QueryContext & ctx) {
    branch_id_t branch = ctx.executionContext.branchId;
    if (branch == master_branch_id) {
        return get_current_master(tid, table);
    }

    const auto version_entry = get_version_entry(tid, table);
    const void * element = get_latest_chain_element(version_entry, table, ctx);
    if (element == nullptr) {
        throw std::runtime_error("no such tuple in the given branch");
    } else if (element == version_entry) {
        return get_current_master(tid, table);
    } else {
        return load_tuple_data(table, get_tuple_ptr(static_cast<const VersionedTupleStorage *>(element)));
    }
}

const void *get_latest_entry(tid_t tid, Table & table, branch_id_t branchId, QueryContext & ctx) {
    ctx.executionContext.branchId = branchId;
    // called once per tuple: switch the lineage only when the scanned branch changes
    if (ctx.executionContext.lineage_branch != branchId) {
        ctx.executionContext.branch_lineage = ctx.executionContext.branch_lineages[branchId];
        ctx.executionContext.reader_label = ctx.executionContext.branch_labels[branchId];
        ctx.executionContext.lineage_branch = branchId;
    }

    if (branchId == master_branch_id) {
        return nullptr;
    }

    const auto version_entry = get_version_entry(tid, table);
    const void * element = get_latest_chain_element(version_entry, table, ctx);
    if (element == nullptr) {
#if LAZY_MEMBERSHIP
        return invisible_version;
#else
        throw std::runtime_error("no such tuple in the given branch");
#endif
    } else if (element == version_entry) {
        return nullptr;
    } else {
        const auto storage = static_cast<const VersionedTupleStorage *>(element);
        return get_tuple_ptr(storage);
    }
}

const void * get_latest_entry_of_reader(tid_t tid, Table & table, branch_id_t branchId, uint64_t reader_label,
                                        const std::unordered_map<branch_id_t, branch_id_t> * lineage,
                                        const uint32_t * limits) {
    if (branchId == master_branch_id) {
        return nullptr;
    }
    const Database & db = table.getDatabase();
    auto visible = [&](branch_id_t branch, branch_id_t creation_ts, ident_t ident) {
#if INTERVAL_VISIBILITY
        return db._identL[ident] <= reader_label && reader_label <= db._identR[ident];
#else
        if (limits != nullptr) return creation_ts < limits[branch];
        if (branch == branchId) return true;
        auto it = lineage->find(branch);
        return it != lineage->end() && creation_ts < it->second;
#endif
    };
    const auto version_entry = get_version_entry(tid, table);
    const void * next = chain_head(version_entry);
    while (next != nullptr) {
        if (next == version_entry) {
            if (visible(version_entry->branch_id, version_entry->creation_ts, version_entry->ident)) return nullptr;
        } else {
            const auto storage = static_cast<const VersionedTupleStorage *>(next);
            if (visible(storage->branch_id, storage->creation_ts, storage->ident)) return get_tuple_ptr(storage);
        }
        next = chain_next(version_entry, next);
    }
#if LAZY_MEMBERSHIP
    return invisible_version;
#else
    throw std::runtime_error("no such tuple in the given branch");
#endif
}

uint32_t get_visible_ranges(tid_t tid, Table & table, const BranchSet & set, bool all_present,
                            const void ** ptrs, uint32_t * lo, uint32_t * hi) {
    const uint32_t k = set.branches.size();
    // ranges of reader positions that have not been given a version yet; per thread to avoid allocations
    thread_local std::vector<std::pair<uint32_t, uint32_t>> pending, rest;
    pending.clear();
    if (all_present) {
        pending.emplace_back(0, k - 1);
    } else {
        auto & membership = table.getMembership();
        for (uint32_t p = 0; p < k; ++p) {
            if (!membership.contains(tid, set.branches[p])) continue;
            if (!pending.empty() && pending.back().second + 1 == p) pending.back().second = p;
            else pending.emplace_back(p, p);
        }
    }

    const Database & db = table.getDatabase();
    const auto version_entry = get_version_entry(tid, table);
    uint32_t parts = 0;
    const void * next = chain_head(version_entry);
    while (next != nullptr && !pending.empty()) {
        ident_t ident;
        branch_id_t branch, creation_ts;
        const void * data;
        if (next == version_entry) {
            ident = version_entry->ident;
            branch = version_entry->branch_id;
            creation_ts = version_entry->creation_ts;
            data = nullptr;
        } else {
            const auto storage = static_cast<const VersionedTupleStorage *>(next);
            ident = storage->ident;
            branch = storage->branch_id;
            creation_ts = storage->creation_ts;
            data = get_tuple_ptr(storage);
        }
        next = chain_next(version_entry, next);
#if COMPILED_LINEAGE
        // without labels, the readers that see the version need not be adjacent: one test per waiting
        // reader, giving runs of readers that see it and runs that wait for an older version
        (void) ident;
        rest.clear();
        auto sees = [&](uint32_t p) { return creation_ts < set.limits[p * set.stride + branch]; };
        for (auto [l, r] : pending) {
            for (uint32_t p = l; p <= r; ) {
                uint32_t q = p;
                while (q <= r && sees(q)) ++q;
                if (q > p) {
                    ptrs[parts] = data;
                    lo[parts] = p;
                    hi[parts] = q - 1;
                    ++parts;
                }
                uint32_t u = q;
                while (u <= r && !sees(u)) ++u;
                if (u > q) rest.emplace_back(q, u - 1);
                p = u;
            }
        }
        std::swap(pending, rest);
        continue;
#else
        (void) branch; (void) creation_ts;
#endif
        // the readers that see this version, unless a newer one shadows it: labels in [l, r]
        const uint32_t a = std::lower_bound(set.labels.begin(), set.labels.end(), db._identL[ident]) - set.labels.begin();
        const uint32_t b = std::upper_bound(set.labels.begin(), set.labels.end(), db._identR[ident]) - set.labels.begin();
        if (a >= b) continue;
        rest.clear();
        for (auto [l, r] : pending) {
            if (r < a || l >= b) {
                rest.emplace_back(l, r);
                continue;
            }
            ptrs[parts] = data;
            lo[parts] = std::max(l, a);
            hi[parts] = std::min(r, b - 1);
            ++parts;
            if (l < a) rest.emplace_back(l, a - 1);
            if (r >= b) rest.emplace_back(b, r);
        }
        std::swap(pending, rest);
    }
#if !LAZY_MEMBERSHIP
    // with lazy membership, the remaining readers contain tuples of their parents that they do not see
    if (!pending.empty()) {
        throw std::runtime_error("no such tuple in the given branch");
    }
#endif
    return parts;
}

bool is_visible(tid_t tid, Table & table, QueryContext & ctx) {
    if (is_marked_as_dangling_tid(tid) && ctx.executionContext.branchId == master_branch_id) {
        return false;
    }
    const auto version_entry = get_version_entry(tid, table);
    const void * element = get_latest_chain_element(version_entry, table, ctx);
    return (element != nullptr);
}

void destroy_chain(VersionEntry * version_entry) {
    const void * next = chain_head(version_entry);
    while (next != nullptr) {
        if (next == version_entry) {
            next = version_entry->next;
        } else {
            const auto storage = static_cast<const VersionedTupleStorage *>(next);
            next = storage->next;
            free_chain_element(storage);
        }
    }
    version_entry->first = version_entry;
    version_entry->next = nullptr;
    version_entry->next_in_branch = nullptr;
}

//-----------------------------------------------------------------------------
// Branch-aware statements: indexes, merge, drop and diff

BranchReader::BranchReader(const Database & db, branch_id_t branch) :
        branch(branch),
        _db(db)
{
    _label = db._identL[db.getIdent(branch)];
    for (branch_id_t current = branch; ; ) {
        branch_id_t parent = db._branches.at(current)->parent_id;
        if (parent == invalid_branch_id) break;
        _lineage[parent] = current;
        current = parent;
    }
}

bool BranchReader::sees(branch_id_t creator, branch_id_t creation_ts, ident_t ident) const {
#if INTERVAL_VISIBILITY
    return _db._identL[ident] <= _label && _label <= _db._identR[ident];
#else
    if (creator == branch) return true;
    auto it = _lineage.find(creator);
    return it != _lineage.end() && creation_ts < it->second;
#endif
}

bool BranchReader::sees(const VersionEntry * entry, const void * element) const {
    if (element == entry) {
        return sees(entry->branch_id, entry->creation_ts, entry->ident);
    }
    auto storage = static_cast<const VersionedTupleStorage *>(element);
    return sees(storage->branch_id, storage->creation_ts, storage->ident);
}

const void * BranchReader::visible(const VersionEntry * entry) const {
    if (branch == master_branch_id) {
        return entry; // master reads in place
    }
    for (const void * element = chain_head(entry); element != nullptr; element = chain_next(entry, element)) {
        if (sees(entry, element)) return element;
    }
    return nullptr;
}

std::unique_ptr<Native::Sql::SqlTuple> load_version(tid_t tid, Table & table, const void * element) {
    if (element == get_version_entry(tid, table)) {
        return get_current_master(tid, table);
    }
    return load_tuple_data(table, get_tuple_ptr(static_cast<const VersionedTupleStorage *>(element)));
}

std::unique_ptr<Native::Sql::SqlTuple> load_tuple_data(const Table & table, const void * data) {
    const auto storage = reinterpret_cast<const VersionedTupleStorage *>(
            static_cast<const uint8_t *>(data) - offsetof(VersionedTupleStorage, data));
    const auto types = table.getTupleType();
    if (storage->columns >= types.size()) {
        return Native::Sql::SqlTuple::load(data, types);
    }
    // the layout of the first columns does not change when columns are added
    const std::vector<Sql::SqlType> stored(types.begin(), types.begin() + storage->columns);
    auto values = std::move(Native::Sql::SqlTuple::load(data, stored)->values);
    for (size_t column = storage->columns; column < types.size(); ++column) {
        ci_p_t ci = table.getCI(column);
        values.push_back(Native::Sql::Value::load(ci->defaultValue.data(), ci->type));
    }
    return std::make_unique<Native::Sql::SqlTuple>(std::move(values));
}

// read instead of the column count of a version by the in-place version, which has all columns
static const uint32_t all_columns = std::numeric_limits<uint32_t>::max();

llvm::Value * genVersionAttributePtr(const Table & table, llvm::Value * tupleData, size_t column) {
    auto & codeGen = getThreadLocalCodeGen();
    auto & context = codeGen.getLLVMContext();
    llvm::Type * tupleTy = Sql::SqlTuple::getType(table.getTupleType());
    llvm::Value * typedTuple = codeGen->CreatePointerCast(tupleData, llvm::PointerType::getUnqual(tupleTy));
    llvm::Value * attribute = codeGen->CreateStructGEP(tupleTy, typedTuple, column);
    ci_p_t ci = table.getCI(column);
    if (!ci->branchLocal) {
        return attribute; // in every version
    }
    llvm::Type * i8Ty = llvm::Type::getInt8Ty(context);
    llvm::Type * i32Ty = llvm::Type::getInt32Ty(context);
    llvm::Type * i32PtrTy = llvm::PointerType::getUnqual(i32Ty);
    const int64_t distance = offsetof(VersionedTupleStorage, data) - offsetof(VersionedTupleStorage, columns);
    llvm::Value * bytes = codeGen->CreatePointerCast(tupleData, LLVMCompat::getInt8PtrTy(context));
    llvm::Value * counted = codeGen->CreatePointerCast(codeGen->CreateGEP(i8Ty, bytes, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), -distance, true)), i32PtrTy);
    llvm::Value * countPtr = codeGen->CreateSelect(codeGen->CreateIsNull(tupleData),
            codeGen->CreatePointerCast(createPointerValue(&all_columns, i32Ty), i32PtrTy), counted);
    llvm::Value * hasColumn = codeGen->CreateICmpUGT(codeGen->CreateLoad(i32Ty, countPtr), llvm::ConstantInt::get(i32Ty, column));
    llvm::Value * defaultPtr = codeGen->CreatePointerCast(createPointerValue(ci->defaultValue.data(), i8Ty), attribute->getType());
    return codeGen->CreateSelect(hasColumn, attribute, defaultPtr);
}

std::unique_ptr<Native::Sql::SqlTuple> complete_tuple(const Table & table,
        std::vector<std::pair<ci_p_t, Native::Sql::value_op_t>> values) {
    std::vector<Native::Sql::value_op_t> all(table.getColumnCount());
    for (auto & [ci, value] : values) {
        all[ci->index] = value ? Native::Sql::toColumnValue(std::move(value), ci->type) : nullptr;
    }
    for (size_t column = 0; column < all.size(); ++column) {
        if (all[column] == nullptr) {
            ci_p_t ci = table.getCI(column);
            all[column] = Native::Sql::Value::load(ci->defaultValue.data(), ci->type);
        }
    }
    return std::make_unique<Native::Sql::SqlTuple>(std::move(all));
}

static bool equal_tuples(const Native::Sql::SqlTuple & a, const Native::Sql::SqlTuple & b) {
    for (size_t i = 0; i < a.values.size(); ++i) {
        if (!a.values[i]->equals(*b.values[i])) return false;
    }
    return true;
}

void build_index(Table & table, BranchIndex & index) {
    BranchReader reader(table.getDatabase(), index.branch);
    auto & membership = table.getMembership();
    for (tid_t tid = 0, size = table._version_mgmt_column.size(); tid < size; ++tid) {
        if (!membership.contains(tid, index.branch)) continue;
        const VersionEntry * entry = table._version_mgmt_column[tid].get();
        const void * element = reader.visible(entry);
        if (element == nullptr) continue;
        index.add(load_version(tid, table, element)->values[index.column]->hash(), tid);
    }
}

// writes a version of the tuple in the branch; predecessor is the version the branch saw before
static void write_version(tid_t tid, branch_id_t branch, Native::Sql::SqlTuple & tuple, Table & table, const void * predecessor) {
    if (branch == master_branch_id) {
        write_in_place(tid, tuple, table);
    } else {
        prepend_version(tid, branch, tuple, table, predecessor);
    }
}

static std::vector<uint8_t> tuple_bytes(Native::Sql::SqlTuple & tuple) {
    std::vector<uint8_t> bytes(tuple.getSize());
    tuple.store(bytes.data());
    return bytes;
}

size_t add_branch_column(Table & table, branch_id_t branch, const std::string & name, Sql::SqlType type,
                         const Native::Sql::Value & defaultValue) {
    std::vector<uint8_t> bytes(defaultValue.getSize());
    defaultValue.store(bytes.data());
    const size_t column = table.addBranchColumn(branch, name, type, bytes);
    // the bases of merges are tuples of all columns as well
    const auto types = table.getTupleType();
    const std::vector<Sql::SqlType> before(types.begin(), types.end() - 1);
    for (auto & [branches, tables] : table.getDatabase()._mergeBases) {
        auto recorded = tables.find(&table);
        if (recorded == tables.end()) continue;
        for (auto & [tid, stored] : recorded->second) {
            auto values = std::move(Native::Sql::SqlTuple::load(stored.data(), before)->values);
            values.push_back(defaultValue.clone());
            Native::Sql::SqlTuple tuple(std::move(values));
            stored = tuple_bytes(tuple);
        }
    }
    return column;
}

// the identifiers of the branch and of its ancestors
/// The identifiers under which each of two branches wrote since they split: those from its current
/// identifier up to, without, the deepest identifier that both descend from, which is returned.
/// Folding (fold_ident()) keeps these paths as short as the forks that are still alive.
static ident_t split_paths(const Database & db, branch_id_t a, branch_id_t b,
                           std::vector<ident_t> & sinceA, std::vector<ident_t> & sinceB) {
    const ident_t identB = db.getIdent(b);
    ident_t common = db.getIdent(a);
    while (!db.isAncestorIdent(common, identB)) {
        sinceA.push_back(common);
        common = db.getIdentParent(common);
    }
    for (ident_t ident = identB; ident != common; ident = db.getIdentParent(ident)) {
        sinceB.push_back(ident);
    }
    return common;
}

std::vector<tid_t> diverging_tuples(Table & table, branch_id_t a, branch_id_t b) {
    const Database & db = table.getDatabase();
    std::vector<ident_t> sinceA, sinceB;
    split_paths(db, a, b, sinceA, sinceB);
    std::vector<tid_t> tids;
    auto add = [&](const std::vector<ident_t> & idents) {
        for (ident_t ident : idents) {
            if (ident < table._writes.size()) {
                tids.insert(tids.end(), table._writes[ident].begin(), table._writes[ident].end());
            }
        }
    };
    add(sinceA);
    add(sinceB);
    std::sort(tids.begin(), tids.end());
    tids.erase(std::unique(tids.begin(), tids.end()), tids.end());
    return tids;
}

// the tuples DIFF and MERGE look at
static std::vector<tid_t> candidates(Table & table, branch_id_t a, branch_id_t b) {
    if (!diff_scans_whole_table) {
        return diverging_tuples(table, a, b);
    }
    std::vector<tid_t> all(table.size());
    std::iota(all.begin(), all.end(), 0);
    return all;
}

MergeResult merge_branch(Database & db, branch_id_t src, branch_id_t dst, MergePolicy policy) {
    BranchReader source(db, src);
    BranchReader target(db, dst);
    auto & bases = db._mergeBases[{src, dst}];

    enum class Kind { Write, Insert, Delete };
    struct Action {
        Table * table;
        tid_t tid;
        Kind kind;
        std::unique_ptr<Native::Sql::SqlTuple> tuple;  // to be written
        std::unique_ptr<Native::Sql::SqlTuple> merged; // the version of src, the base of the next merge
        const void * predecessor;
    };
    std::vector<Action> actions;
    std::vector<std::pair<Table *, size_t>> addedColumns; // to the schema of dst
    MergeResult result;
    const bool resolves = (policy == MergePolicy::KeepTarget || policy == MergePolicy::KeepSource);
    std::vector<ident_t> sourceSince, targetSince;
    const ident_t common = split_paths(db, src, dst, sourceSince, targetSince);

    // tables in the order of their names, so that conflicts are reported deterministically
    std::vector<std::pair<std::string, Table *>> tables;
    for (auto & [name, table] : db.getTables()) tables.emplace_back(name, table.get());
    std::sort(tables.begin(), tables.end());

    for (auto & [name, table] : tables) {
        auto & membership = table->getMembership();
        const auto types = table->getTupleType();
        auto & tableBases = bases[table];
        auto conflict = [&, &name = name](tid_t tid, const std::string & what) {
            result.conflicts.push_back("table " + name + ", tuple " + std::to_string(tid) + ": " + what);
        };
        // the tuples each side wrote (or deleted) since the split
        auto writtenSince = [&](const std::vector<ident_t> & since) {
            std::unordered_set<tid_t> tids;
            for (ident_t ident : since) {
                if (ident < table->_writes.size()) tids.insert(table->_writes[ident].begin(), table->_writes[ident].end());
            }
            return tids;
        };
        const auto sourceWrote = writtenSince(sourceSince), targetWrote = writtenSince(targetSince);

        // the columns of src that the target never had, i.e. that were not added on its path: src or
        // a branch merged into it added them; a column the target lacks otherwise was dropped there
        // (drops are not merged)
        const auto & targetSchema = table->getSchema(dst);
        for (size_t column : table->getSchema(src)) {
            ci_p_t ci = table->getCI(column);
            if (std::find(targetSchema.begin(), targetSchema.end(), column) != targetSchema.end()) continue;
            if (!ci->branchLocal || db.isAncestorIdent(ci->addedUnder, db.getIdent(dst))) continue;
            if (table->findColumn(ci->columnName, dst) != nullptr) {
                result.conflicts.push_back("table " + name + ": column " + ci->columnName + " was added in both branches");
                continue;
            }
            addedColumns.emplace_back(table, column);
        }

        for (tid_t tid : candidates(*table, src, dst)) {
            if (policy == MergePolicy::FirstCommitterWins && sourceWrote.count(tid) > 0 && targetWrote.count(tid) > 0) {
                conflict(tid, "written in both branches since the split");
                continue;
            }
            const VersionEntry * entry = table->_version_mgmt_column[tid].get();
            const void * inSource = membership.contains(tid, src) ? source.visible(entry) : nullptr;
            const void * inTarget = membership.contains(tid, dst) ? target.visible(entry) : nullptr;
            if (inSource == nullptr && inTarget == nullptr) continue;
            if (inSource == inTarget) continue; // both see the same version

            // the base: the version src had when it was merged last, else the newest common version
            std::unique_ptr<Native::Sql::SqlTuple> base;
            auto recorded = tableBases.find(tid);
            if (recorded != tableBases.end()) {
                base = Native::Sql::SqlTuple::load(recorded->second.data(), types);
            } else {
                // a version both see is the base only if both contained the tuple when they split: a side
                // that lacks the tuple and has not written it since did not contain it then
                bool bothContained = (inSource != nullptr || sourceWrote.count(tid) > 0) &&
                                     (inTarget != nullptr || targetWrote.count(tid) > 0);
                for (const void * element = chain_head(entry); bothContained && element != nullptr; element = chain_next(entry, element)) {
                    if (source.sees(entry, element) && target.sees(entry, element)) {
                        // not if the common path deleted the tuple since this version: under the version's
                        // identifier or one below it, up to where the two split
                        const ident_t written = (element == entry) ? entry->ident
                                              : static_cast<const VersionedTupleStorage *>(element)->ident;
                        bool deletedSince = false;
                        for (ident_t ident = common; !deletedSince && db.isAncestorIdent(written, ident); ident = db.getIdentParent(ident)) {
                            deletedSince = ident < table->_deleted.size() && table->_deleted[ident].count(tid) > 0;
                            if (ident == written) break;
                        }
                        if (!deletedSince) base = load_version(tid, *table, element);
                        break;
                    }
                }
                if (!base && inSource == nullptr) continue; // the target has it alone, since the split
            }

            if (inSource != nullptr && inTarget != nullptr) {
                auto s = load_version(tid, *table, inSource);
                auto d = load_version(tid, *table, inTarget);
                std::vector<Native::Sql::value_op_t> merged;
                bool clean = true;
                for (size_t i = 0; i < types.size(); ++i) {
                    const auto & sv = *s->values[i];
                    const auto & dv = *d->values[i];
                    if (sv.equals(dv) || (base && base->values[i]->equals(sv))) {
                        merged.push_back(dv.clone());
                    } else if (base && base->values[i]->equals(dv)) {
                        merged.push_back(sv.clone());
                    } else if (resolves) {
                        merged.push_back(policy == MergePolicy::KeepTarget ? dv.clone() : sv.clone());
                        result.resolved += 1;
                    } else {
                        clean = false;
                        conflict(tid, "column " + table->getCI(i)->columnName + " changed to " + Native::Sql::toString(sv) + " in the source and to " +
                                      Native::Sql::toString(dv) + " in the target" +
                                      (base ? " (was " + Native::Sql::toString(*base->values[i]) + ")" : ""));
                    }
                }
                if (!clean) continue;
                auto tuple = std::make_unique<Native::Sql::SqlTuple>(std::move(merged));
                if (equal_tuples(*tuple, *d)) {
                    // already in the target; remember the version of src for the next merge
                    actions.push_back({table, tid, Kind::Write, nullptr, std::move(s), inTarget});
                } else {
                    actions.push_back({table, tid, Kind::Write, std::move(tuple), std::move(s), inTarget});
                }
            } else if (inSource != nullptr) {
                auto s = load_version(tid, *table, inSource);
                if (!base) {
                    // inserted on the side of src
                    auto copy = Native::Sql::SqlTuple::load(tuple_bytes(*s).data(), types);
                    actions.push_back({table, tid, Kind::Insert, std::move(copy), std::move(s), nullptr});
                } else if (!equal_tuples(*s, *base)) {
                    if (!resolves) {
                        conflict(tid, "deleted in the target and changed in the source");
                    } else if (policy == MergePolicy::KeepTarget) {
                        // stays deleted; the version of src is the base of the next merge
                        actions.push_back({table, tid, Kind::Write, nullptr, std::move(s), nullptr});
                        result.resolved += 1;
                    } else {
                        auto copy = Native::Sql::SqlTuple::load(tuple_bytes(*s).data(), types);
                        actions.push_back({table, tid, Kind::Insert, std::move(copy), std::move(s), nullptr});
                        result.resolved += 1;
                    }
                }
            } else if (base) {
                // only in the target, which has it from a common version: src deleted it
                auto d = load_version(tid, *table, inTarget);
                if (equal_tuples(*d, *base) || policy == MergePolicy::KeepSource) {
                    actions.push_back({table, tid, Kind::Delete, nullptr, nullptr, inTarget});
                    if (!equal_tuples(*d, *base)) result.resolved += 1;
                } else if (policy == MergePolicy::KeepTarget) {
                    result.resolved += 1; // the target keeps its version
                } else {
                    conflict(tid, "deleted in the source and changed in the target");
                }
            }
        }
    }
    if (!result.conflicts.empty()) {
        return result;
    }

    for (auto & action : actions) {
        Table & table = *action.table;
        switch (action.kind) {
            case Kind::Write:
                if (action.tuple) {
                    write_version(action.tid, dst, *action.tuple, table, action.predecessor);
                    result.updated += 1;
                }
                break;
            case Kind::Insert:
                write_version(action.tid, dst, *action.tuple, table, nullptr);
                table.getMembership().set(action.tid, dst, true);
                result.inserted += 1;
                break;
            case Kind::Delete:
                table.getMembership().set(action.tid, dst, false);
                record_write(table, dst, action.tid, true);
                bases[&table].erase(action.tid);
                result.deleted += 1;
                break;
        }
        if (action.merged) {
            bases[&table][action.tid] = tuple_bytes(*action.merged);
        }
    }
    for (auto & [table, column] : addedColumns) {
        table->addToSchema(dst, column);
    }
    return result;
}

std::vector<uintptr_t> compute_version_map(Table & table, branch_id_t branch) {
    BranchReader reader(table.getDatabase(), branch);
    // tuples inserted meanwhile are added in add_version_map
    std::vector<uintptr_t> versions(table.size());
    versions.reserve(table.capacity());
    for (tid_t tid = 0; tid < versions.size(); ++tid) {
        const VersionEntry * entry = table._version_mgmt_column[tid].get();
        const void * element = reader.visible(entry);
        versions[tid] = (element == nullptr) ? 1 : (element == entry) ? 0
                      : reinterpret_cast<uintptr_t>(get_tuple_ptr(static_cast<const VersionedTupleStorage *>(element)));
    }
    return versions;
}

void add_version_map(Table & table, branch_id_t branch, std::vector<uintptr_t> versions) {
    std::lock_guard<std::mutex> lock(table._scanLatch);
    // inserted after the walk by other branches, since the branch is latched: none of their versions is visible
    versions.resize(table._version_mgmt_column.size(), 1);
    if (table._inPlaceWrites != nullptr) {
        // main may have moved an in-place version the walk has seen into the chain
        for (const auto & write : table._inPlaceWrites->writes) {
            if (versions[write.tid] == 0) versions[write.tid] = reinterpret_cast<uintptr_t>(get_tuple_ptr(write.copy));
        }
    }
    // a scan compiled meanwhile may use a map built by another statement; it is maintained, so keep it
    table._versionMaps.emplace(branch, std::move(versions));
}

// scans of other branches may be compiled meanwhile: the map is built first and then added under
// the scan latch
void build_version_map(Table & table, branch_id_t branch) {
    add_version_map(table, branch, compute_version_map(table, branch));
}

void prepare_scan(Table & table, branch_id_t branch) {
    if (branch == master_branch_id || version_map_after_scans == 0) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(table._scanLatch);
        if (table._versionMaps.count(branch) > 0 || ++table._scans[branch] < version_map_after_scans) return;
    }
    build_version_map(table, branch);
}

// removes the elements in garbage from the chain of the tuple and frees them
static void unlink_versions(Table & table, tid_t tid, const std::unordered_set<const void *> & garbage) {
    VersionEntry * entry = table._version_mgmt_column[tid].get();
    std::vector<void *> kept;
    for (const void * element = chain_head(entry); element != nullptr; element = chain_next(entry, element)) {
        if (garbage.count(element) == 0) kept.push_back(const_cast<void *>(element));
    }
    // a reference to a freed version continues with the version its branch saw before it
    auto skipGarbage = [&](const void * element) {
        while (element != nullptr && garbage.count(element) > 0) {
            element = static_cast<const VersionedTupleStorage *>(element)->next_in_branch;
        }
        return element;
    };
    for (size_t i = 0; i < kept.size(); ++i) {
        void * next = (i + 1 < kept.size()) ? kept[i + 1] : nullptr;
        if (kept[i] == entry) {
            entry->next = next;
            entry->next_in_branch = static_cast<VersionedTupleStorage *>(const_cast<void *>(skipGarbage(entry->next_in_branch)));
            entry->prev = (i > 0) ? kept[i - 1] : nullptr;
        } else {
            auto storage = static_cast<VersionedTupleStorage *>(kept[i]);
            storage->next = next;
            storage->next_in_branch = skipGarbage(storage->next_in_branch);
        }
    }
    entry->first = kept.front();
    store_head(table, tid, entry->first == entry ? entry : nullptr);
    for (const void * element : garbage) {
        free_chain_element(element);
    }
}

// the readers whose versions must be kept, as labels in ascending order: the current identifier of
// every branch, and every identifier with descendants of branches in at least two of its subtrees,
// the newest version of which is the base of a merge of branches below them
static std::vector<uint64_t> needed_readers(const Database & db) {
    const size_t count = db.getIdentCount();
    const auto order = db.identPreorder();
    std::vector<bool> current(count, false);
    for (auto & [id, branch] : db._branches) current[branch->ident] = true;
    std::vector<uint32_t> below(count, 0);    // current identifiers in the subtree
    std::vector<uint32_t> children(count, 0); // children whose subtrees hold some
    // children before their parents
    for (size_t i = order.size(); i-- > 0; ) {
        const ident_t ident = order[i];
        below[ident] += current[ident];
        ident_t parent = db.getIdentParent(ident);
        if (parent != invalid_ident && below[ident] > 0) {
            below[parent] += below[ident];
            children[parent] += 1;
        }
    }
    std::vector<uint64_t> labels;
    for (ident_t ident : order) {
        if (current[ident] || children[ident] >= 2) labels.push_back(db._identL[ident]);
    }
    std::sort(labels.begin(), labels.end());
    return labels;
}

// the elements of the chain that some reader sees as its newest version
static void needed_versions(const Database & db, const VersionEntry * entry, const std::vector<uint64_t> & labels,
                            std::unordered_set<const void *> & needed) {
    if (labels.empty()) return;
    std::vector<std::pair<uint32_t, uint32_t>> pending = {{0, static_cast<uint32_t>(labels.size() - 1)}}, rest;
    for (const void * element = chain_head(entry); element != nullptr && !pending.empty(); element = chain_next(entry, element)) {
        const ident_t ident = (element == entry) ? entry->ident : static_cast<const VersionedTupleStorage *>(element)->ident;
        const uint32_t a = std::lower_bound(labels.begin(), labels.end(), db._identL[ident]) - labels.begin();
        const uint32_t b = std::upper_bound(labels.begin(), labels.end(), db._identR[ident]) - labels.begin();
        if (a >= b) continue;
        bool seen = false;
        rest.clear();
        for (auto [l, r] : pending) {
            if (r < a || l >= b) {
                rest.emplace_back(l, r);
                continue;
            }
            seen = true;
            if (l < a) rest.emplace_back(l, a - 1);
            if (r >= b) rest.emplace_back(b, r);
        }
        if (seen) needed.insert(element);
        std::swap(pending, rest);
    }
}

// the writes of an identifier tell two branches apart only if its subtree holds the current
// identifiers of some branches but not of all; frees the other lists
static void prune_writes(Database & db) {
    const size_t count = db.getIdentCount();
    const auto order = db.identPreorder();
    std::vector<uint32_t> below(count, 0);
    for (auto & [id, branch] : db._branches) below[branch->ident] += 1;
    for (size_t i = order.size(); i-- > 0; ) {
        ident_t parent = db.getIdentParent(order[i]);
        if (parent != invalid_ident) below[parent] += below[order[i]];
    }
    // the lists of freed identifiers are empty already
    const uint32_t live = db._branches.size();
    for (auto & [name, table] : db.getTables()) {
        for (ident_t ident : order) {
            if (ident < table->_writes.size() && (below[ident] == 0 || below[ident] == live)) {
                std::vector<tid_t>().swap(table->_writes[ident]);
            }
        }
    }
}

// Folds the identifier child into its parent, after the other children of the parent were dropped and
// while no branch writes under the parent: every reader that sees a version of one sees the other's,
// so the versions written under child are retagged to the parent and child is freed. Its writes are
// found in its lists; they are those of its branch since the fork, which a commit merges anyway.
static void fold_ident(Database & db, ident_t child) {
    const ident_t parent = db.getIdentParent(child);
    for (auto & [name, table] : db.getTables()) {
        table->retagColumns(child, parent);
        if (child >= table->_writes.size()) continue;
        std::vector<tid_t> tids;
        tids.swap(table->_writes[child]);
        std::sort(tids.begin(), tids.end());
        tids.erase(std::unique(tids.begin(), tids.end()), tids.end());
        std::unordered_set<tid_t> deleted;
        if (child < table->_deleted.size()) deleted.swap(table->_deleted[child]);
        if (table->_deleted.size() <= parent) table->_deleted.resize(parent + 1);
        auto & parentWrites = table->_writes[parent];
        for (tid_t tid : tids) {
            VersionEntry * entry = table->_version_mgmt_column[tid].get();
            for (const void * element = chain_head(entry); element != nullptr; element = chain_next(entry, element)) {
                ident_t & ident = (element == entry) ? entry->ident
                                : const_cast<VersionedTupleStorage *>(static_cast<const VersionedTupleStorage *>(element))->ident;
                if (ident == child) ident = parent;
            }
            if (chain_head(entry) == entry) store_head(*table, tid, entry);
            table->getMembership().retagEntries(tid, child, parent);
            // the writes of child came after those of the parent
            parentWrites.push_back(tid);
            if (deleted.count(tid) > 0) table->_deleted[parent].insert(tid);
            else table->_deleted[parent].erase(tid);
        }
    }
    db.foldIdent(child);
}

size_t drop_branches(Database & db, const std::vector<branch_id_t> & branches) {
    std::unordered_set<branch_id_t> dropped(branches.begin(), branches.end());
    size_t freed = 0;

    // the identifiers under which tuples were written since the fork of the subtree: those of the
    // dropped branches, each a path from its current identifier up to its first, and those of the
    // parent after the fork, up to the identifier it forked the subtree from
    const branch_id_t root = branches.front();
    const branch_id_t parent = db._branches.at(root)->parent_id;
    std::vector<ident_t> dead;
    ident_t fork = invalid_ident;
    for (branch_id_t branch : branches) {
        ident_t ident = db.getIdent(branch);
        for (; ident != invalid_ident && db.getIdentBranch(ident) == branch; ident = db.getIdentParent(ident)) dead.push_back(ident);
        if (branch == root) fork = ident;
    }
    const std::unordered_set<ident_t> deadSet(dead.begin(), dead.end());
    std::vector<ident_t> written = dead;
    for (ident_t ident = db.getIdent(parent); ident != invalid_ident && ident != fork; ident = db.getIdentParent(ident)) {
        written.push_back(ident);
    }
    std::unordered_map<Table *, std::vector<tid_t>> candidates;
    for (auto & [name, table] : db.getTables()) {
        auto & tids = candidates[table.get()];
        for (ident_t ident : written) {
            if (ident < table->_writes.size()) tids.insert(tids.end(), table->_writes[ident].begin(), table->_writes[ident].end());
        }
        std::sort(tids.begin(), tids.end());
        tids.erase(std::unique(tids.begin(), tids.end()), tids.end());
    }

    // the versions written by dropped branches are seen by dropped branches only
    std::unordered_set<const void *> garbage;
    for (auto & [table, tids] : candidates) {
        for (tid_t tid : tids) {
            VersionEntry * entry = table->_version_mgmt_column[tid].get();
            garbage.clear();
            for (const void * element = chain_head(entry); element != nullptr; element = chain_next(entry, element)) {
                if (element != entry && dropped.count(static_cast<const VersionedTupleStorage *>(element)->branch_id) > 0) {
                    garbage.insert(element);
                }
            }
            if (!garbage.empty()) {
                unlink_versions(*table, tid, garbage);
                freed += garbage.size();
            }
            // an in-place version of a dropped branch heads a tuple it inserted and no other branch
            // contains: no reader sees it, also once its identifier is reused
            if (deadSet.count(entry->ident) > 0) {
                entry->ident = db.deadIdent();
                if (chain_head(entry) == entry) store_head(*table, tid, entry);
            }
        }
    }
    for (auto & [name, table] : db.getTables()) {
        for (ident_t ident : dead) {
            table->retagColumns(ident, db.deadIdent()); // in no branch's path, as merges ask
            if (ident < table->_writes.size()) std::vector<tid_t>().swap(table->_writes[ident]);
            if (ident < table->_deleted.size()) std::unordered_set<tid_t>().swap(table->_deleted[ident]);
        }
    }
    for (auto it = db._mergeBases.begin(); it != db._mergeBases.end(); ) {
        bool obsolete = dropped.count(it->first.first) > 0 || dropped.count(it->first.second) > 0;
        it = obsolete ? db._mergeBases.erase(it) : std::next(it);
    }
    db.removeBranches(branches, dead);

    // the parent forked the subtree under an identifier it no longer writes with: if no other child
    // of it is left, the parent's later identifiers fold into it, so that paths do not grow with the
    // number of branches that were forked and dropped
    if (fork != invalid_ident && db.getIdentChildren(fork).size() == 1 && db.getIdent(parent) != fork) {
        const ident_t child = db.getIdentChildren(fork).front();
        if (db.getIdentBranch(child) == db.getIdentBranch(fork)) fold_ident(db, child);
    }

    // versions the remaining branches do not need, such as those the parent kept for the dropped ones
    if (collect_garbage) {
        prune_writes(db);
        const auto labels = needed_readers(db);
        std::unordered_set<const void *> needed;
        for (auto & [table, tids] : candidates) {
            for (tid_t tid : tids) {
                const VersionEntry * entry = table->_version_mgmt_column[tid].get();
                needed.clear();
                needed_versions(db, entry, labels, needed);
                garbage.clear();
                for (const void * element = chain_head(entry); element != nullptr; element = chain_next(entry, element)) {
                    if (element != entry && needed.count(element) == 0) garbage.insert(element);
                }
                if (garbage.empty()) continue;
                unlink_versions(*table, tid, garbage);
                freed += garbage.size();
            }
        }
    }
    return freed;
}

void diff_branches(Table & table, branch_id_t a, branch_id_t b, std::vector<DiffEntry> & out) {
    Database & db = table.getDatabase();
    const BranchReader readers[2] = {BranchReader(db, a), BranchReader(db, b)};
    auto & membership = table.getMembership();
    // the columns both branches have, which DIFF returns
    std::vector<size_t> common;
    const auto & schemaB = table.getSchema(b);
    for (size_t column : table.getSchema(a)) {
        if (std::find(schemaB.begin(), schemaB.end(), column) != schemaB.end()) common.push_back(column);
    }
    auto equal = [&](const Native::Sql::SqlTuple & x, const Native::Sql::SqlTuple & y) {
        for (size_t column : common) {
            if (!x.values[column]->equals(*y.values[column])) return false;
        }
        return true;
    };
    for (tid_t tid : candidates(table, a, b)) {
        const VersionEntry * entry = table._version_mgmt_column[tid].get();
        const void * seen[2];
        for (int side = 0; side < 2; ++side) {
            seen[side] = membership.contains(tid, readers[side].branch) ? readers[side].visible(entry) : nullptr;
        }
        if (seen[0] == seen[1]) continue; // the same version, or in neither branch
        if (seen[0] != nullptr && seen[1] != nullptr &&
            equal(*load_version(tid, table, seen[0]), *load_version(tid, table, seen[1]))) {
            continue; // different versions with the same values in the columns of both
        }
        for (uint32_t side = 0; side < 2; ++side) {
            if (seen[side] == nullptr) continue;
            const void * data = (seen[side] == entry) ? nullptr : get_tuple_ptr(static_cast<const VersionedTupleStorage *>(seen[side]));
            out.push_back({tid, data, side});
        }
    }
}
