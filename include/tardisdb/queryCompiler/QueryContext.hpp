
#pragma once

#include "codegen/CodeGen.hpp"
#include "foundations/Database.hpp"
#include "foundations/InformationUnit.hpp"

#include <unordered_map>

#include <boost/dynamic_bitset.hpp>

struct ExecutionResource {
    virtual ~ExecutionResource() { }
};

struct ExecutionContext {
    std::vector<std::unique_ptr<ExecutionResource>> resources;
    void acquireResource(std::unique_ptr<ExecutionResource> && resource);

    bool overflowFlag = false;
    branch_id_t branchId = 0;
    std::unordered_map<std::string,branch_id_t> branchIds;
    std::unordered_map<branch_id_t, branch_id_t> branch_lineage; // mapping: parent -> offspring
    boost::dynamic_bitset<> branch_lineage_bitset;
};

// TODO distinguish between Compilation and Runtime context
struct QueryContext {
    QueryContext(Database & pDB) :
            db(pDB),
            codeGen(getThreadLocalCodeGen())
    { }

    // compilation related
    Database & db;
    IUFactory iuFactory;
    CodeGen & codeGen;

//    std::unique_ptr<Algebra::Logical::Operator> logicalTree;
//    std::unique_ptr<Algebra::Physical::Operator> physicalTree;

    using scope_op_t = std::unique_ptr<std::unordered_map<std::string, iu_p_t>>;

    // root scope
    std::unordered_map<std::string, iu_p_t> scope;

    uint32_t operatorUID = 0;

    // runtime related
    ExecutionContext executionContext;
};
