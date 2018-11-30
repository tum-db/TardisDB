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

    Database & db = table.getDatabase();
    auto storage = create_chain_element(table, tuple.getSize());

    tid_t tid = table._dangling_version_mgmt_column.size();
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
    version_entry->next_in_branch = storage;

    /*
    not used in this case:
    version_entry->branch_id;
    version_entry->branch_visibility;
    */

    version_entry->branch_visibility.set(branch);

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

void update_tuple(tid_t tid, Native::Sql::SqlTuple & tuple, Table & table, QueryContext & ctx) {
    branch_id_t branch = ctx.executionContext.branchId;

    if (is_marked_as_dangling_tid(tid) && branch == master_branch_id) {
        throw std::runtime_error("no such tuple in the given branch");
    }

    auto version_entry = get_version_entry(tid, table);
    if (!version_entry->branch_visibility.test(branch)) {
        throw std::runtime_error("no such tuple in the given branch");
    }

    Database & db = table.getDatabase();
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

const void * get_latest_chain_element(const VersionEntry * version_entry, Table & table, QueryContext & ctx) {
    branch_id_t branch = ctx.executionContext.branchId;

    if (branch == master_branch_id) {
        return version_entry;
    }

    const void * next = version_entry->first;
    while (next != nullptr) {
        if (next == version_entry) {
            // this is the current master branch
            if (is_visible(version_entry, ctx)) {
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

//template<SqlType...>
/*
SqlType getBoolTy(bool nullable = false);

SqlType getDateTy(bool nullable = false);

SqlType getIntegerTy(bool nullable = false);

SqlType getNumericFullLengthTy(uint8_t precision, bool nullable = false);

SqlType getNumericTy(uint8_t length, uint8_t precision, bool nullable = false);

SqlType getCharTy(uint32_t length, bool nullable = false);

SqlType getVarcharTy(uint32_t capacity, bool nullable = false);

SqlType getTimestampTy(bool nullable = false);

SqlType getTextTy(bool inplace, bool nullable = false);
*/
#if 0
class Register {
    static constexpr size_t max_size = std::max({
        sizeof(Native::Sql::Bool),
        sizeof(Native::Sql::Date),
        sizeof(Native::Sql::Integer),
        sizeof(Native::Sql::NullableValue),
//        sizeof(Native::Sql::Char), // TODO
//        sizeof(Native::Sql::Varchar), // TODO
        sizeof(Native::Sql::Timestamp),
        sizeof(Native::Sql::Text)
    });
    Sql::SqlType type;
    size_t size;
    uint8_t data[max_size];
//    void store(Native::Sql::Value ...);
    void load_from(const void * ptr) {
        std::memcpy(data, ptr, size);
    }


};
#endif

class Register {
    Sql::SqlType type;
    bool is_null;
    Native::Sql::Bool bool_value;
    Native::Sql::Date date_value;

    void load_from(const void * ptr) {
        if (is_null) {
            throw NotImplementedException();
        }

        switch (type.typeID) {
            case Sql::SqlType::TypeID::UnknownID:
                assert(false);
            case Sql::SqlType::TypeID::BoolID:
                throw NotImplementedException(); // TODO

/*
                new (&bool_value) ();
*/

            case Sql::SqlType::TypeID::IntegerID:
                return Integer::load(ptr);
    //        case SqlType::TypeID::VarcharID:
    //        case SqlType::TypeID::CharID:
            case Sql::SqlType::TypeID::NumericID:
                return Numeric::load(ptr, type);
            case Sql::SqlType::TypeID::DateID:
                return Date::load(ptr);
            case Sql::SqlType::TypeID::TimestampID:
                return Timestamp::load(ptr);
            default:
                unreachable();
        }
    }

    Native::Sql::Value & get_value() {
        switch (type.typeID) {
            case Sql::SqlType::TypeID::UnknownID:
                assert(false);
            case Sql::SqlType::TypeID::BoolID:
                return bool_value;
        }
    }
};

std::unique_ptr<Native::Sql::SqlTuple> get_current_master(tid_t tid, Table & table) {
    std::vector<Native::Sql::value_op_t> values;
    auto & tuple_type = table.getTupleType();
    for (size_t i = 0; i < tuple_type.size(); ++i) {
        const void * ptr = table.getColumn(i).at(tid);
        values.push_back(Native::Sql::Value::load(ptr, tuple_type[tid]));
    }
    return std::make_unique<Native::Sql::SqlTuple>(std::move(values));
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
        auto & tuple_type = table.getTupleType();
        return Native::Sql::SqlTuple::load(tuple_ptr, tuple_type);
    }
}
