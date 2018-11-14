#include "foundations/version_management.hpp"

#include "utils/general.hpp"
#include "utils/tagged_ptr.hpp"

static VersionedTupleStorage * create_chain_element(Table & table, size_t tuple_size) {

    Database & db = table.getDatabase();

    branch_id_t largest_branch_id = db.getLargesBranchId();
    size_t branch_indicator_vec_size = DynamicBitvector::get_alloc_size(largest_branch_id);

    size_t size = sizeof(VersionedTupleStorage) + branch_indicator_vec_size + tuple_size;
    void * mem = std::malloc(size);
    VersionedTupleStorage * storage = new (mem) VersionedTupleStorage();
    storage->tuple_offset = branch_indicator_vec_size;

    DynamicBitvector::construct(largest_branch_id, storage->data);

    return storage;
}

inline DynamicBitvector * get_branch_indicator_vector(VersionedTupleStorage * storage) {
    return reinterpret_cast<DynamicBitvector *>(storage->data);
}

inline const DynamicBitvector * get_branch_indicator_vector(const VersionedTupleStorage * storage) {
    return reinterpret_cast<const DynamicBitvector *>(storage->data);
}

inline void * get_tuple_ptr(VersionedTupleStorage * storage) {
    return (storage->data + storage->tuple_offset);
}

inline const void * get_tuple_ptr(const VersionedTupleStorage * storage) {
    return (storage->data + storage->tuple_offset);
}

tid_t insert_tuple(Native::Sql::SqlTuple & tuple, branch_id_t branch, Table & table) {
    // TODO

    auto storage = create_chain_element(table, tuple.getSize());


}

void update_tuple(tid_t tid, Native::Sql::SqlTuple & tuple, branch_id_t branch, Table & table) {
    // TODO
}

void delete_tuple(tid_t tid, Native::Sql::SqlTuple & tuple, branch_id_t branch, Table & table) {
    // TODO
}

std::unique_ptr<Native::Sql::SqlTuple> get_latest_tuple(tid_t tid, branch_id_t branch, Table & table) {
    if (branch == master_branch_id) {
        throw 0; // use a regular scan
    }

    void * next = table.mv_begin[tid];

    while (next != nullptr) {
        tagged_ptr chain(next);

        if (likely(chain.is_tagged())) {
            const auto storage = static_cast<const VersionedTupleStorage *>(chain.untag().get());
            const auto branch_indicator_vec = get_branch_indicator_vector(storage);
            if (branch_indicator_vec->test(branch)) {
                const void * tuple_ptr = get_tuple_ptr(storage);
                auto & tuple_type = table.getTupleType();
                return Native::Sql::SqlTuple::load(tuple_ptr, tuple_type);
            }
            next = storage->next;
        } else {
            const auto & branchBitmap = table.getBranchBitmap();
            if (branchBitmap.isSet(tid, branch)) {
                // TODO
                throw 0;
            }
            next = table.mv_next[tid];
        }
    }
}
