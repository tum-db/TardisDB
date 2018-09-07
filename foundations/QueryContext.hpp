
#pragma once

#include "foundations/Database.hpp"
#include "foundations/InformationUnit.hpp"

using scope_level_t = size_t;

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
//    bool overflow = false;
    //...
};

static bool overflowFlag = false;

void addToScope(QueryContext & context, iu_p_t iu, const std::string & symbol);

void addToScope(QueryContext & context, Algebra::Logical::TableScan & scan);

void addToScope(QueryContext & context, Algebra::Logical::TableScan & scan, const std::string & prefix);

iu_p_t lookup(QueryContext & context, const std::string & symbol);

void genOverflowException();

void genOverflowEvaluation();
