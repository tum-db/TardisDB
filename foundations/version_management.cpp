#include "foundations/version_management.hpp"

#include "foundations/exceptions.hpp"
#include "utils/general.hpp"
#include "utils/tagged_ptr.hpp"

static std::unique_ptr<DynamicBitvector> construct_lineage_vector(QueryContext & ctx) {
}

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

    Database & db = table.getDatabase();

    size_t size = sizeof(VersionedTupleStorage) + + tuple_size;
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

tid_t mark_as_dangling_tid(tid_t tid) {
    return (tid | static_cast<decltype(tid)>(1) << (8*sizeof(decltype(tid))-1));
}

tid_t unmark_dangling_tid(tid_t tid) {
    return (tid & ~(static_cast<decltype(tid)>(1) << (8*sizeof(decltype(tid))-1)));
}

tid_t is_marked_as_dangling_tid(tid_t tid) {
    return (0 < tid & static_cast<decltype(tid)>(1) << (8*sizeof(decltype(tid))-1));
}

tid_t insert_tuple(Native::Sql::SqlTuple & tuple, Table & table, QueryContext & ctx) {
    branch_id_t branch = ctx.executionContext.branchId;

    if (branch == master_branch_id) {
        throw 0; // use the regular insert method
    }

    // TODO
    Database & db = table.getDatabase();
    auto storage = create_chain_element(table, tuple.getSize());

    tid_t tid = table.dangling_chains.size();
    tid = mark_as_dangling_tid(tid);

    // branch visibility
    storage->branch_id = branch;
    storage->creation_ts = db.getLargestBranchId();

    tuple.store(get_tuple_ptr(storage));

    return tid;
}

#if 0
void update_tuple(tid_t tid, Native::Sql::SqlTuple & tuple, Table & table, QueryContext & ctx) {

    branch_id_t branch = ctx.executionContext.branchId;

    // TODO

    if (is_marked_as_dangling_tid(tid) && branch == master_branch_id) {
        throw std::runtime_error("no such tuple in the given branch");
    }


    VersionedTupleStorage * head;
    if (is_marked_as_dangling_tid(tid)) {
        //
        tid_t unmarked = unmark_dangling_tid(tid);
        head = static_cast<VersionedTupleStorage *>(table.dangling_chains[unmarked]);
    } else {
//        head = static_cast<VersionedTupleStorage *>(table.mv_begin[tid]);
        head = static_cast<VersionedTupleStorage *>(table._version_mgmt_column[tid]->first);
    }


    while (!is_visible(storage, ctx)) {
        // TODO
        void * next = head->next;
        // TODO inspect tag

    }
}
#endif

VersionEntry * get_version_entry(tid_t tid, Table & table) {
    if (is_marked_as_dangling_tid(tid)) {
        tid_t unmarked = unmark_dangling_tid(tid);
        return table._dangling_version_mgmt_column[unmarked].get();
    } else {
        return table._version_mgmt_column[tid].get();
    }
}

void update_tuple(tid_t tid, Native::Sql::SqlTuple & tuple, Table & table, QueryContext & ctx) {

    branch_id_t branch = ctx.executionContext.branchId;

    // TODO

    if (is_marked_as_dangling_tid(tid) && branch == master_branch_id) {
        throw std::runtime_error("no such tuple in the given branch");
    }

    auto version_entry = get_version_entry(tid, table);
    if (!version_entry->branch_visibility.test(branch)) {
        throw std::runtime_error("no such tuple in the given branch");
    }

    Database & db = table.getDatabase();
    if (branch == master_branch_id) {
        auto current_tuple = get_latest_tuple(tid, table, ctx);

        auto storage = create_chain_element(table, current_tuple->getSize());

        // branch visibility
        storage->branch_id = master_branch_id;
        storage->creation_ts = db.getLargestBranchId();

        current_tuple->store(get_tuple_ptr(storage));

        if (untagged_ptr(version_entry->first) == master...) {

        } else {
            
        }

    } else {

        auto next_in_branch = get_latest_chain_element(version_entry, table, ctx);

        auto storage = create_chain_element(table, tuple.getSize());

        // branch visibility
        storage->branch_id = branch;
        storage->creation_ts = db.getLargestBranchId();

        tuple.store(get_tuple_ptr(storage));

        storage->next = version_entry->first;
        storage->next_in_branch = next_in_branch;

        version_entry->first = storage;
    }
}

tid_t delete_tuple(tid_t tid, Native::Sql::SqlTuple & tuple, Table & table, QueryContext & ctx) {
    // TODO
    throw NotImplementedException();
}
#if 0
const VersionedTupleStorage * get_latest_chain_element(tid_t tid, Table & table, QueryContext & ctx) {
    branch_id_t branch = ctx.executionContext.branchId;

    auto version_entry = get_version_entry(tid, table);

    if (!version_entry->branch_visibility.test(branch)) {
        throw std::runtime_error("no such tuple in the given branch");
    }


}
#endif
const VersionedTupleStorage * get_latest_chain_element(VersionEntry * version_entry, Table & table, QueryContext & ctx) {
    branch_id_t branch = ctx.executionContext.branchId;



}

template<SqlType...>
class Register {
    std::max(...);
};

std::unique_ptr<Native::Sql::SqlTuple> get_latest_tuple(tid_t tid, Table & table, QueryContext & ctx) {
    branch_id_t branch = ctx.executionContext.branchId;
    if (branch == master_branch_id) {
        std::vector<Native::Sql::value_op_t> values;
        auto & tuple_type = table.getTupleType();
        for (size_t i = 0; i < tuple_type.size(); ++i) {
            const void * ptr = table.getColumn(i).at(tid);
            values.push_back(Native::Sql::Value::load(ptr, tuple_type[tid]));
        }
        return std::make_unique<Native::Sql::SqlTuple>(std::move(values));
    }

    const auto entry = get_version_entry(tid, table);
    const void * next = entry->first;
    while (next != nullptr) {
        tagged_ptr chain(next);

        if (likely(chain.is_tagged())) {
            // master column
            if (is_visible(entry, ctx)) {
                // TODO
                throw 0;
            }
            next = entry->next;

        } else {
            const auto storage = static_cast<const VersionedTupleStorage *>(chain.untag().get());
            if (is_visible(*storage, ctx)) {
                const void * tuple_ptr = get_tuple_ptr(storage);
                auto & tuple_type = table.getTupleType();
                return Native::Sql::SqlTuple::load(tuple_ptr, tuple_type);
            }
            next = storage->next;
        }
    }
}
