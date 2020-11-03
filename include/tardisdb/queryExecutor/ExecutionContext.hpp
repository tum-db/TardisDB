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

struct ExecutionContext {
    std::vector<std::unique_ptr<ExecutionResource>> resources;
    void acquireResource(std::unique_ptr<ExecutionResource> && resource);

    bool overflowFlag = false;
    branch_id_t branchId = 0;
    std::unordered_map<branch_id_t, branch_id_t> branch_lineage; // mapping: parent -> offspring
    boost::dynamic_bitset<> branch_lineage_bitset;
};

#endif //PROTODB_EXECUTIONCONTEXT_HPP
