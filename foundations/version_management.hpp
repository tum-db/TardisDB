#pragma once

#include "foundations/Database.hpp"
#include "foundations/QueryContext.hpp"
#include "native/sql/SqlValues.hpp"
#include "native/sql/SqlTuple.hpp"
#include "utils/optimistic_lock.hpp"

#include <boost/dynamic_bitset.hpp>

//#include <bitset>

class Table;

/* TODO
implement C++ SQL type interface

reuse branch identifiers?
*/

// updating the master branches moves the whole structure entirely to the right

/*
in Table:
std::vector<VersionedTupleStorage> newest;
*/

/*
namespace Cpp {
namespace Sql {

class SqlTuple {
};

class SqlValue {
};

};
};

struct ExecutionContext {
    std::unordered_map<branch_id_t, branch_id_t> branch_lineage; // mapping: parent -> offspring
};
*/

struct DynamicBitvector {
    unsigned cnt; // bit cnt
//    std::bitset bits;
    uint64_t data[0];

    static size_t get_alloc_size(unsigned branch_cnt);

    static DynamicBitvector & construct(unsigned branch_cnt, void * dst);

    void set(unsigned idx);

    bool test(unsigned idx) const;
};

// similar to VersionedTupleStorage; used by the current 'master' branch entry
struct VersionEntry {
    void * first = nullptr; // use pointer tagging
    void * next = nullptr; // use pointer tagging
    VersionedTupleStorage * next_in_branch;
    branch_id_t branch_id;
    branch_id_t creation_ts; // latest branch id during the time of creation (same as the length of the branch bitvector)
//    int64_t lock;
    opt_lock::lock_t lock;
    boost::dynamic_bitset<> branch_visibility;
};

struct VersionedTupleStorage {
    const void * next = nullptr; // use pointer tagging
    const void * next_in_branch = nullptr;
    branch_id_t branch_id;
    branch_id_t creation_ts; // latest branch id during the time of creation
//    size_t tuple_offset;
    uint8_t data[0];
};

branch_id_t create_branch(std::string name);

// FIXME tuple has to exist in the master branch!
tid_t insert_tuple(Native::Sql::SqlTuple & tuple, Table & table, QueryContext & ctx);

void update_tuple(tid_t tid, Native::Sql::SqlTuple & tuple, Table & table, QueryContext & ctx);

// the entire version chain has to be deleted
// the tuple has to be relocated iff branch==master
tid_t delete_tuple(tid_t tid, Native::Sql::SqlTuple & tuple, Table & table, QueryContext & ctx);

tid_t merge_tuple(branch_id_t src_branch, branch_id_t dst_branch, tid_t tid, QueryContext ctx);

std::unique_ptr<Native::Sql::SqlTuple> get_latest_tuple(tid_t tid, Table & table, QueryContext & ctx);

template<typename Consumer>
void get_latest_tuple(QueryContext & ctx, tid_t tid, Table & table, std::vector<ci_p_t> & to_produce, Consumer consumer);

template<typename Consumer>
void get_latest_tuple(QueryContext & ctx, tid_t tid, Table & table, std::vector<ci_p_t> & to_produce, Consumer consumer) {
    // set up
    std::vector<Register> registers;
    registers.resize(to_produce.size());
    for (auto ci : to_produce) {

    }
    if (is_marked_as_dangling_tid(tid)) {

    } else {
        for (;;) {
            // load
//            Native::Sql::Value::load(ptr, type)
            registers[i].load_from(ptr);
        }
        consumer(registers)
    }
}


// revision_offset == 0 => latest revision
std::unique_ptr<Native::Sql::SqlTuple> get_tuple(tid_t tid, unsigned revision_offset, Table & table, QueryContext & ctx);

//void scan_relation(branch_id_t branch, Table & table, std::function<> consumer);
template<typename Consumer>
void scan_relation(branch_id_t branch, Table & table, std::vector<ci_p_t> & to_produce, Consumer consumer);

template<typename Consumer>
void scan_relation(branch_id_t branch, Table & table, std::vector<ci_p_t> & to_produce, Consumer consumer) {
    if (branch == master_branch_id) {

    } else {
        
    }
}
