#include "foundations/version_management.hpp"

#include <algorithm>
#include <iostream>
#include <unordered_set>

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
    index_tuple(table, branch, tid, tuple);

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

// master updates in place; the previous in-place version moves into the chain
static void write_in_place(tid_t tid, Native::Sql::SqlTuple & tuple, Table & table) {
    const branch_id_t branch = master_branch_id;
    Database & db = table.getDatabase();
    auto version_entry = get_version_entry(tid, table);
    {
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
    }
    index_tuple(table, branch, tid, tuple);
}

// other branches prepend their version to the chain; predecessor is the version the branch saw before
static void prepend_version(tid_t tid, branch_id_t branch, Native::Sql::SqlTuple & tuple, Table & table, const void * predecessor) {
    Database & db = table.getDatabase();
    auto version_entry = get_version_entry(tid, table);
    {
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
    index_tuple(table, branch, tid, tuple);
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
#if !LAZY_MEMBERSHIP
    // with lazy membership, the remaining readers contain tuples of their parents that they do not see
    if (!pending.empty()) {
        throw std::runtime_error("no such tuple in the given branch");
    }
#endif
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

static const void * next_chain_element(const VersionEntry * entry, const void * element) {
    return (element == entry) ? entry->next : static_cast<const VersionedTupleStorage *>(element)->next;
}

const void * BranchReader::visible(const VersionEntry * entry) const {
    if (branch == master_branch_id) {
        return entry; // master reads in place
    }
    for (const void * element = entry->first; element != nullptr; element = next_chain_element(entry, element)) {
        if (sees(entry, element)) return element;
    }
    return nullptr;
}

std::unique_ptr<Native::Sql::SqlTuple> load_version(tid_t tid, Table & table, const void * element) {
    if (element == get_version_entry(tid, table)) {
        return get_current_master(tid, table);
    }
    return Native::Sql::SqlTuple::load(get_tuple_ptr(static_cast<const VersionedTupleStorage *>(element)), table.getTupleType());
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

MergeResult merge_branch(Database & db, branch_id_t src, branch_id_t dst) {
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
    MergeResult result;

    // tables in the order of their names, so that conflicts are reported deterministically
    std::vector<std::pair<std::string, Table *>> tables;
    for (auto & [name, table] : db.getTables()) tables.emplace_back(name, table.get());
    std::sort(tables.begin(), tables.end());

    for (auto & [name, table] : tables) {
        auto & membership = table->getMembership();
        const auto columnNames = table->getColumnNames();
        const auto types = table->getTupleType();
        auto & tableBases = bases[table];
        auto conflict = [&, &name = name](tid_t tid, const std::string & what) {
            result.conflicts.push_back("table " + name + ", tuple " + std::to_string(tid) + ": " + what);
        };

        for (tid_t tid = 0, size = table->_version_mgmt_column.size(); tid < size; ++tid) {
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
                for (const void * element = entry->first; element != nullptr; element = next_chain_element(entry, element)) {
                    if (source.sees(entry, element) && target.sees(entry, element)) {
                        base = load_version(tid, *table, element);
                        break;
                    }
                }
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
                    } else {
                        clean = false;
                        conflict(tid, "column " + columnNames[i] + " changed to " + Native::Sql::toString(sv) + " in the source and to " +
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
                    conflict(tid, "deleted in the target and changed in the source");
                }
            } else if (base) {
                // only in the target, which has it from a common version: src deleted it
                auto d = load_version(tid, *table, inTarget);
                if (equal_tuples(*d, *base)) {
                    actions.push_back({table, tid, Kind::Delete, nullptr, nullptr, inTarget});
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
                bases[&table].erase(action.tid);
                result.deleted += 1;
                break;
        }
        if (action.merged) {
            bases[&table][action.tid] = tuple_bytes(*action.merged);
        }
    }
    return result;
}

size_t drop_branches(Database & db, const std::vector<branch_id_t> & branches) {
    std::unordered_set<branch_id_t> dropped(branches.begin(), branches.end());
    size_t freed = 0;
    for (auto & [name, table] : db.getTables()) {
        for (tid_t tid = 0, size = table->_version_mgmt_column.size(); tid < size; ++tid) {
            VersionEntry * entry = table->_version_mgmt_column[tid].get();

            // the versions written by dropped branches are seen by dropped branches only
            std::vector<void *> kept;
            std::unordered_map<const void *, const void *> garbage; // freed version -> its next_in_branch
            for (const void * element = entry->first; element != nullptr; element = next_chain_element(entry, element)) {
                if (element != entry) {
                    auto storage = static_cast<const VersionedTupleStorage *>(element);
                    if (dropped.count(storage->branch_id) > 0) {
                        garbage[element] = storage->next_in_branch;
                        continue;
                    }
                }
                kept.push_back(const_cast<void *>(element));
            }
            if (garbage.empty()) continue;

            auto skipGarbage = [&](const void * element) {
                for (auto it = garbage.find(element); it != garbage.end(); it = garbage.find(element)) {
                    element = it->second;
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
            table->_head_ident[tid] = (entry->first == entry) ? entry->ident : invalid_ident;

            for (auto & [element, _] : garbage) {
                std::free(const_cast<void *>(element));
            }
            freed += garbage.size();
        }
    }
    for (auto it = db._mergeBases.begin(); it != db._mergeBases.end(); ) {
        bool obsolete = dropped.count(it->first.first) > 0 || dropped.count(it->first.second) > 0;
        it = obsolete ? db._mergeBases.erase(it) : std::next(it);
    }
    db.removeBranches(branches);
    return freed;
}

void diff_branches(Table & table, branch_id_t a, branch_id_t b, std::vector<DiffEntry> & out) {
    Database & db = table.getDatabase();
    const BranchReader readers[2] = {BranchReader(db, a), BranchReader(db, b)};
    auto & membership = table.getMembership();
    for (tid_t tid = 0, size = table._version_mgmt_column.size(); tid < size; ++tid) {
        const VersionEntry * entry = table._version_mgmt_column[tid].get();
        const void * seen[2];
        for (int side = 0; side < 2; ++side) {
            seen[side] = membership.contains(tid, readers[side].branch) ? readers[side].visible(entry) : nullptr;
        }
        if (seen[0] == seen[1]) continue; // the same version, or in neither branch
        if (seen[0] != nullptr && seen[1] != nullptr &&
            equal_tuples(*load_version(tid, table, seen[0]), *load_version(tid, table, seen[1]))) {
            continue; // different versions with the same values
        }
        for (uint32_t side = 0; side < 2; ++side) {
            if (seen[side] == nullptr) continue;
            const void * data = (seen[side] == entry) ? nullptr : get_tuple_ptr(static_cast<const VersionedTupleStorage *>(seen[side]));
            out.push_back({tid, data, side});
        }
    }
}
