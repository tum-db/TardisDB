#include "foundations/Database.hpp"
#include "semanticAnalysis/SQLParser.hpp"
#include "algebra/logical/operators.hpp"
#include <string>
#include <memory>
#include <unordered_map>
#include <unordered_set>

using namespace Algebra::Logical;

std::unique_ptr<Result> sql_to_plan(QueryContext& context, std::string sql);

struct QueryPlan {
    SQLParserResult parser_result;

    std::unordered_map<std::string, iu_p_t> scope;
    std::unordered_map<std::string, Table *> attributeToTable;
    // binding -> scan
    std::unordered_map<std::string, std::unique_ptr<Operator>> scans;
    std::unordered_map<std::string, Select *> selects;
    // binding -> subtree
    std::unordered_map<std::string, std::unique_ptr<Operator>> dangling_subtrees;
    std::unique_ptr<Operator> tree;
};

void construct_scans(QueryContext& context, QueryPlan & plan);
void construct_selects(QueryContext & context, QueryPlan & plan);
std::unique_ptr<Result> construct_projection(QueryContext & context, QueryPlan & plan);