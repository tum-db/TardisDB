#pragma once

#include "foundations/Database.hpp"
#include "foundations/QueryContext.hpp"
#include "native/sql/SqlValues.hpp"
#include "native/sql/SqlTuple.hpp"

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
    void * first; // use pointer tagging
    void * next; // use pointer tagging
    VersionedTupleStorage * next_in_branch;
    branch_id_t branch_id;
    branch_id_t creation_ts; // latest branch id during the time of creation (same as the length of the branch bitvector)
};


struct VersionedTupleStorage {
    void * next; // use pointer tagging
    void * next_in_branch;
    branch_id_t branch_id;
//    branch_id_t creation_ts; // latest branch id during the time of creation
    size_t tuple_offset;
//    DynamicBitvector branchIndicators;
//    void * tuple;
    uint8_t data[0];
};

branch_id_t create_branch(std::string name);

// FIXME tuple has to exist in the master branch!
tid_t insert_tuple(Native::Sql::SqlTuple & tuple, Table & table, QueryContext & ctx);

void update_tuple(tid_t tid, Native::Sql::SqlTuple & tuple, Table & table, QueryContext & ctx);

// FIXME
// tuple aus master branch loeschen fuehrt zu konflikt!
void delete_tuple(tid_t tid, Native::Sql::SqlTuple & tuple, Table & table, QueryContext & ctx);


std::unique_ptr<Native::Sql::SqlTuple> get_latest_tuple(tid_t tid, Table & table, QueryContext & ctx);

// revision_offset == 0 => latest revision
std::unique_ptr<Native::Sql::SqlTuple> get_tuple(tid_t tid, unsigned revision_offset, Table & table, QueryContext & ctx);

void scan_relation(branch_id_t branch, Table & table, std::function<> consumer);
