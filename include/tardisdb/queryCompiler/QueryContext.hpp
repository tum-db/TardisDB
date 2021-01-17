#ifndef PROTODB_QUERYCONTEXT_HPP
#define PROTODB_QUERYCONTEXT_HPP

#include "queryExecutor/ExecutionContext.hpp"
#if USE_HYRISE
#include "SQLParser.h"
#else
#include "sqlParser/ParserResult.hpp"
#endif
#include "semanticAnalyser/AnalyzingContext.hpp"
#include "queryExecutor/ExecutionContext.hpp"
#include "codegen/CodeGen.hpp"
#include "foundations/Database.hpp"

struct QueryContext {
    QueryContext(Database & pDB) :
            analyzingContext(pDB),
            codeGen(getThreadLocalCodeGen()) {}

    CodeGen & codeGen;

    static void constructBranchLineages(std::set<branch_id_t> &branches, QueryContext &context);

    ExecutionContext executionContext;
    semanticalAnalysis::AnalyzingContext analyzingContext;
#if USE_HYRISE
    hsql::SQLParserResult hyriseResult;

    static std::tuple<std::string,int,int> convertDataType(hsql::ColumnType type);
    static void collectTables(std::vector<semanticalAnalysis::Relation> &relations, hsql::TableRef *tableRef);
    static std::string expressionValueToString(hsql::Expr *expr);
    static void collectSelections(std::vector<std::pair<semanticalAnalysis::Column,std::string>> &selections, hsql::Expr *whereClause);
    static void collectJoinConditions(std::vector<std::pair<semanticalAnalysis::Column,semanticalAnalysis::Column>> &joinConditions, hsql::Expr *whereClause);
    static void convertToParserResult(semanticalAnalysis::SQLParserResult &dest, hsql::SQLParserResult &source);
#else
    tardisParser::ParsingContext parsingContext;

    static void convertToParserResult(semanticalAnalysis::SQLParserResult &dest, tardisParser::ParsingContext &source);
#endif
};

#endif // PROTODB_QUERYCONTEXT_HPP