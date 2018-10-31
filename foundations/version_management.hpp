#pragma once

#include "foundations/Database.hpp"

#include <bitset>

/* TODO
implement C++ SQL type interface

reuse branch identifiers?
*/

// updating the master branches moves the whole structure entirely to the right

/*
in Table:
std::vector<VersionedTupleStorage> newest;
*/


namespace Cpp {
namespace Sql {

class SqlTuple {
};

class SqlValue {
};

};
};


struct DynamicBitvector {
    unsigned cnt; // bit cnt
    std::bitset bits;

    void set(unsigned idx);
    bool test(unsigned idx) {
        if (idx >= cnt) {
            return true;
        }
        return bits.test(idx);
    }
};

struct VersionedTupleStorage {
    void * next; // use pointer tagging
    DynamicBitvector branchIndicators;
//    void * tuple;
    uint8_t data[0];
};

branch_id_t create_branch(std::string name);

tid_t insert(std::vector<Cpp::Sql::SqlValue> tuple, branch_id_t branch, Table & table);

void update(tid_t tid, std::vector<Cpp::Sql::SqlValue> tuple, branch_id_t branch, Table & table);

// FIXME
// tuple aus master branch loeschen fuehrt zu konflikt!

void delete_tuple(tid_t tid, std::vector<Cpp::Sql::SqlValue> tuple, branch_id_t branch, Table & table);


Cpp::Sql::SqlTuple get_latest_tuple(tid_t tid, branch_id_t branch, Tale & table);

// revision_offset == 0 => latest revision
Cpp::Sql::SqlTuple get_tuple(tid_t tid, branch_id_t branch, unsigned revision_offset, Table & table);

void scan_relation(branch_id_t branch, Table & table, std::function<> consumer);
