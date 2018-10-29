#pragma once

#include <foundations/Database.hpp>

/* TODO
implement C++ SQL type interface
*/

// updating the master branches moves the whole structure entirely to the right

/*
in Table:
std::vector<VersionedTupleStorage> newest;
*/

struct DynamicBitvector {

};

struct VersionedTupleStorage {
    void * next; // use pointer tagging

    DynamicBitvector branchIndicators;
};


tid_t insert(std::vector<SqlValue> tuple, branch_id_t branch, Table & table);

void update(tid_t tid, std::vector<SqlValue> tuple, branch_id_t branch, Table & table);


// FIXME
// tuple aus master branch loeschen fuehrt zu konflikt!

void delete_tuple(tid_t tid, std::vector<SqlValue> tuple, branch_id_t branch, Table & table);
