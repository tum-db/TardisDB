
#pragma once

#if USE_HYRISE
#include "SQLParser.h"
#else
#include "sqlParser/ParserResult.hpp"
#endif
#include "semanticAnalyser/AnalyzingContext.hpp"
#include "codegen/CodeGen.hpp"
#include "foundations/Database.hpp"
#include "semanticAnalyser/InformationUnit.hpp"
#include "semanticAnalyser/IUFactory.hpp"

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
    semanticalAnalysis::AnalyzingContext analyzingContext;
#if USE_HYRISE
    hsql::SQLParserResult hyriseResult;
#else
    tardisParser::ParsingContext parsingContext;
#endif

#if USE_HYRISE
    static std::tuple<std::string,int,int> convertDataType(hsql::ColumnType type);
    static void collectTables(std::vector<semanticalAnalysis::Relation> &relations, hsql::TableRef *tableRef);
    static std::string expressionValueToString(hsql::Expr *expr);
    static void collectSelections(std::vector<std::pair<semanticalAnalysis::Column,std::string>> &selections, hsql::Expr *whereClause);
    static void collectJoinConditions(std::vector<std::pair<semanticalAnalysis::Column,semanticalAnalysis::Column>> &joinConditions, hsql::Expr *whereClause);
    static void convertToParserResult(semanticalAnalysis::SQLParserResult &dest, hsql::SQLParserResult &source);
#else
    static void convertToParserResult(semanticalAnalysis::SQLParserResult &dest, tardisParser::ParsingContext &source);
#endif
};
