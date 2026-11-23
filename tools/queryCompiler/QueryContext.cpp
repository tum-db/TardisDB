#include "queryCompiler/QueryContext.hpp"

void ExecutionContext::acquireResource(std::unique_ptr<ExecutionResource> && resource)
{
    resources.push_back(std::move(resource));
}

void QueryContext::emitToSink(void * context, void * tuple)
{
    static_cast<QueryContext *>(context)->tupleSink(static_cast<Native::Sql::SqlTuple *>(tuple));
}

void QueryContext::constructBranchLineages(std::set<branch_id_t> &branches, QueryContext &context) {
    for (auto &branch : branches) {
        context.analyzingContext.db.constructBranchLineage(branch,context.executionContext);
    }
}

void QueryContext::convertToParserResult(semanticalAnalysis::SQLParserResult &dest, tardisParser::ParsingContext &source) {
    dest = std::move((semanticalAnalysis::SQLParserResult&)source);
    switch (source.opType) {
        case tardisParser::ParsingContext::Unkown:
            break;
        case tardisParser::ParsingContext::CreateTable:
            dest.opType = semanticalAnalysis::SQLParserResult::OpType::CreateTable;
            break;
        case tardisParser::ParsingContext::CreateBranch:
            dest.opType = semanticalAnalysis::SQLParserResult::OpType::CreateBranch;
            break;
        case tardisParser::ParsingContext::Branch:
            dest.opType = semanticalAnalysis::SQLParserResult::OpType::Branch;
            break;
        case tardisParser::ParsingContext::Insert:
            dest.opType = semanticalAnalysis::SQLParserResult::OpType::Insert;
            break;
        case tardisParser::ParsingContext::Select:
            dest.opType = semanticalAnalysis::SQLParserResult::OpType::Select;
            break;
        case tardisParser::ParsingContext::Update:
            dest.opType = semanticalAnalysis::SQLParserResult::OpType::Update;
            break;
        case tardisParser::ParsingContext::Delete:
            dest.opType = semanticalAnalysis::SQLParserResult::OpType::Delete;
            break;
        case tardisParser::ParsingContext::Copy:
            dest.opType = semanticalAnalysis::SQLParserResult::OpType::Copy;
            break;
        case tardisParser::ParsingContext::CreateIndex:
            dest.opType = semanticalAnalysis::SQLParserResult::OpType::CreateIndex;
            break;
        case tardisParser::ParsingContext::DropBranch:
            dest.opType = semanticalAnalysis::SQLParserResult::OpType::DropBranch;
            break;
        case tardisParser::ParsingContext::MergeBranch:
            dest.opType = semanticalAnalysis::SQLParserResult::OpType::MergeBranch;
            break;
        case tardisParser::ParsingContext::AlterTable:
            dest.opType = semanticalAnalysis::SQLParserResult::OpType::AlterTable;
            break;
    }
    source = tardisParser::ParsingContext();
}
