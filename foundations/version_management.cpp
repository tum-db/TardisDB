#include "foundations/version_management.hpp"

#include "utils/tagged_ptr.hpp"

tid_t insert(std::vector<Cpp::Sql::SqlValue> tuple, branch_id_t branch, Table & table) {
    // TODO
}

void update(tid_t tid, std::vector<Cpp::Sql::SqlValue> tuple, branch_id_t branch, Table & table) {
    // TODO
}

void delete_tuple(tid_t tid, std::vector<Cpp::Sql::SqlValue> tuple, branch_id_t branch, Table & table) {
    // TODO
}

Cpp::Sql::SqlTuple get_latest_tuple(tid_t tid, branch_id_t branch, Tale & table) {
    if (branch == master_branch_id) {
        throw 0; // use a regular scan
    }

    void * next = table.mv_begin[tid];

    while (next != nullptr) {
        tagged_ptr chain(next);

        if (likely(chain.is_tagged()) {
            const auto storage = static_cast<const VersionedTupleStorage *>(chain.untag())
            if (storage.branchIndicators.test(branch)) {
                return Cpp::Sql::SqlTuple::load(storage.tuple);
            }
            next = storage->...;
        } else {
            const auto & branchBitmap = table.getBranchBitmap();
            if (branchBitmap.isSet(tid, branch)) {
                // TODO
            }
            next = table.mv_next[tid];
        }


    }
}
