
#pragma once

#include "codegen/CodeGen.hpp"
#include "foundations/Database.hpp"
#include "foundations/InformationUnit.hpp"

#include <unordered_map>

using scope_level_t = size_t;

struct ExecutionResource {
    virtual ~ExecutionResource() { }
};

struct ExecutionContext {
    std::vector<std::unique_ptr<ExecutionResource>> resources;
    void acquireResource(std::unique_ptr<ExecutionResource> && resource);

    bool overflowFlag = false;
    branch_id_t branchId = 0;
    std::unordered_map<branch_id_t, branch_id_t> branch_lineage; // mapping: parent -> offspring
};

// TODO distinguish between Compilation and Runtime context
struct QueryContext {
    QueryContext(Database & pDB) :
            db(pDB),
            codeGen(getThreadLocalCodeGen())
    { }

//    ExecutionContext & getExecutionContext();

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

static bool overflowFlag = false;

void addToScope(QueryContext & context, iu_p_t iu, const std::string & symbol);

void addToScope(QueryContext & context, Algebra::Logical::TableScan & scan);

void addToScope(QueryContext & context, Algebra::Logical::TableScan & scan, const std::string & prefix);

iu_p_t lookup(QueryContext & context, const std::string & symbol);

void genOverflowException();

void genOverflowEvaluation();
