#include "foundations/version_management.hpp"

#include <iostream>

#include "foundations/exceptions.hpp"
#include "utils/general.hpp"

template<typename T>
static bool is_visible(const T & elem, QueryContext & ctx) {
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
        version_entry->next = version_entry->first;             // next should point to the old head of the chain
        version_entry->next_in_branch = storage;                // next_in_branch points to the inserted storage entry
        version_entry->first = version_entry;                   // the head now points again to the version_entry
        version_entry->branch_id = branch;
        version_entry->creation_ts = db.getLargestBranchId();

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
    }
}

void update_tuple_with_binding(tid_t tid, std::string *binding, Native::Sql::SqlTuple & tuple, Table & table, QueryContext & ctx) {
    if (binding != nullptr) {
        ctx.executionContext.branchId = ctx.executionContext.branchIds[*binding];
    }
    return update_tuple(tid,tuple,table,ctx);
}

void delete_tuple(tid_t tid, Table & table, QueryContext & ctx) {
    branch_id_t branch = ctx.executionContext.branchId;

    table.removeRowForBranch(tid,branch);
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

std::unique_ptr<Native::Sql::SqlTuple> get_latest_tuple_with_binding(std::string *binding, tid_t tid, Table & table, QueryContext & ctx) {
    if (binding != nullptr) {
        ctx.executionContext.branchId = ctx.executionContext.branchIds[*binding];
    }
    ctx.db.constructBranchLineage(ctx.executionContext.branchId, ctx.executionContext);
    return std::move(get_latest_tuple(tid,table,ctx));
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

uint8_t is_visible_with_binding(tid_t tid, std::string *binding, Table & table, QueryContext & ctx) {
    if (binding != nullptr) {
        ctx.executionContext.branchId = ctx.executionContext.branchIds[*binding];
    }
    ctx.db.constructBranchLineage(ctx.executionContext.branchId, ctx.executionContext);
    if (is_visible(tid,table,ctx)) {
        return 1;
    } else {
        return 0;
    }
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
