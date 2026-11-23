#ifndef PROTODB_QUERYCONTEXT_HPP
#define PROTODB_QUERYCONTEXT_HPP

#include "queryExecutor/ExecutionContext.hpp"
#include "sqlParser/ParserResult.hpp"
#include "semanticAnalyser/AnalyzingContext.hpp"
#include "codegen/CodeGen.hpp"
#include "foundations/Database.hpp"

#include <functional>

namespace Native { namespace Sql { class SqlTuple; } }

struct QueryContext {
    QueryContext(Database & pDB) :
            analyzingContext(pDB),
            codeGen(getThreadLocalCodeGen()) {}

    CodeGen & codeGen;

    static void constructBranchLineages(std::set<branch_id_t> &branches, QueryContext &context);

    ExecutionContext executionContext;
    semanticalAnalysis::AnalyzingContext analyzingContext;

    /// set while code is generated into a morsel function that several workers run
    bool parallelPipeline = false;

    /// receives the result tuples instead of the callback of the query function, so that every
    /// statement (e.g. of the sessions of a server) has its own receiver; called under the result
    /// latch of a parallel pipeline; the tuple is refilled for the next row, so the receiver copies
    /// what it keeps
    std::function<void(Native::Sql::SqlTuple *)> tupleSink;
    static void emitToSink(void * context, void * tuple);
    tardisParser::ParsingContext parsingContext;

    static void convertToParserResult(semanticalAnalysis::SQLParserResult &dest, tardisParser::ParsingContext &source);
};

#endif // PROTODB_QUERYCONTEXT_HPP