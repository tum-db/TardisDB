#pragma once

#include "foundations/Database.hpp"
#include "native/sql/SqlValues.hpp"
#include "native/sql/SqlTuple.hpp"

//#include <bitset>

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
*/

struct DynamicBitvector {
    unsigned cnt; // bit cnt
//    std::bitset bits;
    uint32_t data[0];

    static size_t get_alloc_size(unsigned branch_cnt);

    static DynamicBitvector & construct(unsigned branch_cnt, void * dst);

    void set(unsigned idx);

    bool test(unsigned idx) const {
        if (idx >= cnt) {
            return true;
        }
        return bits.test(idx);
    }
};

struct VersionedTupleStorage {
    void * next; // use pointer tagging
    size_t tuple_offset;
//    DynamicBitvector branchIndicators;
//    void * tuple;
    uint8_t data[0];
};

branch_id_t create_branch(std::string name);

tid_t insert_tuple(Native::Sql::SqlTuple & tuple, branch_id_t branch, Table & table);

void update_tuple(tid_t tid, Native::Sql::SqlTuple & tuple, branch_id_t branch, Table & table);

// FIXME
// tuple aus master branch loeschen fuehrt zu konflikt!

void delete_tuple(tid_t tid, Native::Sql::SqlTuple & tuple, branch_id_t branch, Table & table);


std::unique_ptr<Native::Sql::SqlTuple> get_latest_tuple(tid_t tid, branch_id_t branch, Table & table);

// revision_offset == 0 => latest revision
std::unique_ptr<Native::Sql::SqlTuple> get_tuple(tid_t tid, branch_id_t branch, unsigned revision_offset, Table & table);

void scan_relation(branch_id_t branch, Table & table, std::function<> consumer);
