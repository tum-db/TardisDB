//
// Created by Blum Thomas on 20.10.20.
//

#ifndef PROTODB_EXECUTIONCONTEXT_HPP
#define PROTODB_EXECUTIONCONTEXT_HPP

#include <vector>
#include "foundations/Database.hpp"
#include <boost/dynamic_bitset.hpp>


struct ExecutionResource {
    virtual ~ExecutionResource() { }
};

/// A counter that compiled code reaches by address, unlike a stack slot of the query function
struct CounterResource : public ExecutionResource {
    uint64_t value = 0;
};

/// An array of pointers that compiled code loops over, e.g. the tuples of an INSERT
struct PointerArrayResource : public ExecutionResource {
    std::vector<void *> pointers;
};

struct ExecutionContext {
    std::vector<std::unique_ptr<ExecutionResource>> resources;
    void acquireResource(std::unique_ptr<ExecutionResource> && resource);

    uint64_t affectedRows = 0; // the tuples an INSERT, UPDATE or DELETE wrote
    branch_id_t branchId = 0;
    std::unordered_map<branch_id_t, branch_id_t> branch_lineage; // mapping: parent -> offspring
    std::unordered_map<branch_id_t,std::unordered_map<branch_id_t, branch_id_t>> branch_lineages;
    boost::dynamic_bitset<> branch_lineage_bitset;
    branch_id_t lineage_branch = invalid_branch_id; // the branch whose lineage branch_lineage holds

    // interval visibility: label of the reader's identifier and the label arrays of the database
    uint64_t reader_label = 0;
    std::unordered_map<branch_id_t, uint64_t> branch_labels;
    const uint64_t * ident_l = nullptr;
    const uint64_t * ident_r = nullptr;
};

#endif //PROTODB_EXECUTIONCONTEXT_HPP
