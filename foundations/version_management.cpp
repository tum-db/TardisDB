#include "foundations/version_management.hpp"

#include <algorithm>
#include <iostream>

#include "foundations/exceptions.hpp"
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

static VersionedTupleStorage * create_chain_element(Table & table, size_t tuple_size) {
    size_t size = sizeof(VersionedTupleStorage) + tuple_size;
    void * mem = std::malloc(size);
    VersionedTupleStorage * storage = new (mem) VersionedTupleStorage();
    return storage;
}

inline void * get_tuple_ptr(VersionedTupleStorage * storage) {
    return storage->data;
}

inline const void * get_tuple_ptr(const VersionedTupleStorage * storage) {
    return storage->data;
}

tid_t insert_tuple(Native::Sql::SqlTuple & tuple, Table & table, QueryContext & ctx) {
    tid_t tid;
    branch_id_t branch = ctx.executionContext.branchId;
    Database & db = table.getDatabase();
    size_t branch_cnt = 1 + db.getLargestBranchId();

    tid = table._version_mgmt_column.size();

    table._version_mgmt_column.push_back(std::make_unique<VersionEntry>());
    VersionEntry * version_entry = table._version_mgmt_column.back().get();
    version_entry->first = version_entry;
    version_entry->next = nullptr;
    version_entry->next_in_branch = nullptr;

    // branch visibility
    version_entry->branch_id = branch;
    version_entry->branch_visibility.resize(branch_cnt, false);
    version_entry->branch_visibility.set(branch);
    version_entry->creation_ts = db.getLargestBranchId();
    version_entry->ident = db.getIdent(branch);
    table._head_ident.push_back(version_entry->ident);

    // store tuple
    table.addRow(branch);
    size_t column_idx = 0;
    for (auto & value : tuple.values) {
        void * ptr = const_cast<void *>(table.getColumn(column_idx).back());
        value->store(ptr);
        column_idx += 1;
    }

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

void update_tuple(tid_t tid, Native::Sql::SqlTuple & tuple, Table & table, QueryContext & ctx) {
    branch_id_t branch = ctx.executionContext.branchId;

    if (is_marked_as_dangling_tid(tid) && branch == master_branch_id) {
        throw std::runtime_error("no such tuple in the given branch");
    }

    Database & db = table.getDatabase();
    auto version_entry = get_version_entry(tid, table);
    if (branch == master_branch_id) {
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
        table._head_ident[tid] = version_entry->ident;

        // new master
        update_master(tid, tuple, table);
    } else {
        auto predecessor = get_latest_chain_element(version_entry, table, ctx);
        if (predecessor == nullptr) {
            throw std::runtime_error("no such tuple in the given branch");
        }

        auto storage = create_chain_element(table, tuple.getSize());

        // branch visibility
        storage->branch_id = branch;
        storage->creation_ts = db.getLargestBranchId();
        storage->ident = db.getIdent(branch);
        size_t branch_cnt = 1 + db.getLargestBranchId();
        if (version_entry->branch_visibility.size() < branch_cnt) {
            version_entry->branch_visibility.resize(branch_cnt, false);
        }
        version_entry->branch_visibility.set(branch);

        tuple.store(get_tuple_ptr(storage));

        storage->next = version_entry->first;
        storage->next_in_branch = predecessor;

        // If the head equals the version_entry set the prev pointer of the version_entry to the address of the
        // created storage entry
        if (version_entry->first == version_entry) version_entry->prev = storage;
        version_entry->first = storage;
        if (!is_marked_as_dangling_tid(tid)) {
            table._head_ident[tid] = invalid_ident;
        }
    }
}

void update_tuple_with_branchId(tid_t tid, branch_id_t branchId, Native::Sql::SqlTuple & tuple, Table & table, QueryContext & ctx) {
    ctx.executionContext.branchId = branchId;
    return update_tuple(tid,tuple,table,ctx);
}

void delete_tuple(tid_t tid, Table & table, QueryContext & ctx) {
    branch_id_t branch = ctx.executionContext.branchId;

    table.removeRowForBranch(tid,branch);
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

    const void * next = version_entry->first;
    while (next != nullptr) {
        if (next == version_entry) {
            // this is the current master branch
            if (is_visible(*version_entry, ctx)) {
                return version_entry;
            }
            next = version_entry->next;
        } else {
            const auto storage = static_cast<const VersionedTupleStorage *>(next);
            if (is_visible(*storage, ctx)) {
                return storage;
            }
            next = storage->next;
        }
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

const void * get_chain_element(const VersionEntry * version_entry, unsigned revision_offset, Table & table, QueryContext & ctx) {
    const void * latest = get_latest_chain_element(version_entry, table, ctx);

    auto current = latest;
    auto current_offset = revision_offset;
    while (current_offset != 0 && current != nullptr) {
        if (current == version_entry) {
            current = version_entry->next_in_branch;
        } else {
            current = static_cast<const VersionedTupleStorage *>(current)->next_in_branch;
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
        const auto storage = static_cast<const VersionedTupleStorage *>(element);
        const void * tuple_ptr = get_tuple_ptr(storage);
        auto tuple_type = table.getTupleType();
        return Native::Sql::SqlTuple::load(tuple_ptr, tuple_type);
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
        throw std::runtime_error("no such tuple in the given branch");
    } else if (element == version_entry) {
        return nullptr;
    } else {
        const auto storage = static_cast<const VersionedTupleStorage *>(element);
        return get_tuple_ptr(storage);
    }
}

const void * get_latest_entry_of_reader(tid_t tid, Table & table, branch_id_t branchId, uint64_t reader_label,
                                        const std::unordered_map<branch_id_t, branch_id_t> * lineage) {
    if (branchId == master_branch_id) {
        return nullptr;
    }
    const Database & db = table.getDatabase();
    auto visible = [&](branch_id_t branch, branch_id_t creation_ts, ident_t ident) {
#if INTERVAL_VISIBILITY
        return db._identL[ident] <= reader_label && reader_label <= db._identR[ident];
#else
        if (branch == branchId) return true;
        auto it = lineage->find(branch);
        return it != lineage->end() && creation_ts < it->second;
#endif
    };
    const auto version_entry = get_version_entry(tid, table);
    const void * next = version_entry->first;
    while (next != nullptr) {
        if (next == version_entry) {
            if (visible(version_entry->branch_id, version_entry->creation_ts, version_entry->ident)) return nullptr;
            next = version_entry->next;
        } else {
            const auto storage = static_cast<const VersionedTupleStorage *>(next);
            if (visible(storage->branch_id, storage->creation_ts, storage->ident)) return get_tuple_ptr(storage);
            next = storage->next;
        }
    }
    throw std::runtime_error("no such tuple in the given branch");
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
        auto & bitmap = table.getBranchBitmap();
        for (uint32_t p = 0; p < k; ++p) {
            if (!bitmap.isSet(tid, set.branches[p])) continue;
            if (!pending.empty() && pending.back().second + 1 == p) pending.back().second = p;
            else pending.emplace_back(p, p);
        }
    }

    const Database & db = table.getDatabase();
    const auto version_entry = get_version_entry(tid, table);
    uint32_t parts = 0;
    const void * next = version_entry->first;
    while (next != nullptr && !pending.empty()) {
        ident_t ident;
        const void * data;
        if (next == version_entry) {
            ident = version_entry->ident;
            data = nullptr;
            next = version_entry->next;
        } else {
            const auto storage = static_cast<const VersionedTupleStorage *>(next);
            ident = storage->ident;
            data = get_tuple_ptr(storage);
            next = storage->next;
        }
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
    if (!pending.empty()) {
        throw std::runtime_error("no such tuple in the given branch");
    }
    return parts;
}

std::unique_ptr<Native::Sql::SqlTuple> get_tuple(tid_t tid, unsigned revision_offset, Table & table, QueryContext & ctx) {
    const auto version_entry = get_version_entry(tid, table);
    const void * element = get_chain_element(version_entry, revision_offset, table, ctx);
    if (element == nullptr) {
        throw std::runtime_error("no such tuple in the given branch");
    } else if (element == version_entry) {
        return get_current_master(tid, table);
    } else {
        const auto storage = static_cast<const VersionedTupleStorage *>(element);
        const void * tuple_ptr = get_tuple_ptr(storage);
        auto tuple_type = table.getTupleType();
        return Native::Sql::SqlTuple::load(tuple_ptr, tuple_type);
    }
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
    const void * next = version_entry->first;
    while (next != nullptr) {
        if (next == version_entry) {
            next = version_entry->next;
        } else {
            const auto storage = static_cast<const VersionedTupleStorage *>(next);
            next = storage->next;
            std::free(const_cast<VersionedTupleStorage *>(storage));
        }
    }
    version_entry->first = version_entry;
    version_entry->next = nullptr;
    version_entry->next_in_branch = nullptr;
}
