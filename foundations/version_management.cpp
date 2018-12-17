#include "foundations/version_management.hpp"

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

    if (branch == master_branch_id) {
        tid = table._version_mgmt_column.size();

        table._version_mgmt_column.push_back(std::make_unique<VersionEntry>());
        VersionEntry * version_entry = table._dangling_version_mgmt_column.back().get();
        version_entry->first = version_entry;
        version_entry->next = nullptr;
        version_entry->next_in_branch = nullptr;

        // branch visibility
        version_entry->branch_id = master_branch_id;
        version_entry->branch_visibility.set(master_branch_id);
        version_entry->creation_ts = db.getLargestBranchId();

        // store tuple
        table.addRow();
        size_t column_idx = 0;
        for (auto & value : tuple.values) {
            void * ptr = const_cast<void *>(table.getColumn(column_idx).back());
            value->store(ptr);
            column_idx += 1;
        }
    } else {
        auto storage = create_chain_element(table, tuple.getSize());

        tid = table._dangling_version_mgmt_column.size();
        tid = mark_as_dangling_tid(tid);

        // branch visibility
        storage->branch_id = branch;
        storage->creation_ts = db.getLargestBranchId();

        tuple.store(get_tuple_ptr(storage));

        // version entry
        table._dangling_version_mgmt_column.push_back(std::make_unique<VersionEntry>());
        VersionEntry * version_entry = table._dangling_version_mgmt_column.back().get();
        version_entry->first = storage;
        version_entry->next = storage;
        version_entry->next_in_branch = nullptr;
        /*
        not used in this case:
        version_entry->branch_id;
        */
        version_entry->branch_visibility.set(branch);
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

static std::unique_ptr<Native::Sql::SqlTuple> get_current_master(tid_t tid, Table & table) {
    std::vector<Native::Sql::value_op_t> values;
    auto tuple_type = table.getTupleType();
    for (size_t i = 0; i < tuple_type.size(); ++i) {
        const void * ptr = table.getColumn(i).at(tid);
        values.push_back(Native::Sql::Value::load(ptr, tuple_type[tid]));
    }
    return std::make_unique<Native::Sql::SqlTuple>(std::move(values));
}

void update_tuple(tid_t tid, Native::Sql::SqlTuple & tuple, Table & table, QueryContext & ctx) {
    branch_id_t branch = ctx.executionContext.branchId;

    if (is_marked_as_dangling_tid(tid) && branch == master_branch_id) {
        throw std::runtime_error("no such tuple in the given branch");
    }

    Database & db = table.getDatabase();
    auto version_entry = get_version_entry(tid, table);
    if (branch == master_branch_id) {
        auto current_tuple = get_current_master(tid, table);

        auto storage = create_chain_element(table, current_tuple->getSize());

        // branch visibility
        storage->branch_id = master_branch_id;
        storage->creation_ts = db.getLargestBranchId();

        current_tuple->store(get_tuple_ptr(storage));

        // version entry update
        if (version_entry->first != version_entry) {
            // chain head != current master
           version_entry->first = version_entry; 
        }
        version_entry->next = storage;
        version_entry->next_in_branch = storage;
    } else {
        auto predecessor = get_latest_chain_element(version_entry, table, ctx);
        if (predecessor == nullptr) {
            throw std::runtime_error("no such tuple in the given branch");
        }

        auto storage = create_chain_element(table, tuple.getSize());

        // branch visibility
        storage->branch_id = branch;
        storage->creation_ts = db.getLargestBranchId();
        version_entry->branch_visibility.set(branch);

        tuple.store(get_tuple_ptr(storage));

        storage->next = version_entry->first;
        storage->next_in_branch = predecessor;

        version_entry->first = storage;
    }
}

tid_t delete_tuple(tid_t tid, Native::Sql::SqlTuple & tuple, Table & table, QueryContext & ctx) {
    // TODO
    throw NotImplementedException();
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
