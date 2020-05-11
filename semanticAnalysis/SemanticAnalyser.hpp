#include "foundations/Database.hpp"
#include "semanticAnalysis/SQLParser.hpp"
#include "algebra/logical/operators.hpp"
#include <string>
#include <memory>
#include <unordered_map>
#include <unordered_set>

using namespace Algebra::Logical;

class SemanticAnalyser {
public:
    static std::unique_ptr<Result> parse_and_construct_tree(QueryContext& context, std::string sql);

private:
    struct QueryPlan {
        SQLParserResult parser_result;

        std::unordered_map<std::string, iu_p_t> ius;
        std::unordered_map<std::string, Table *> iuNameToTable;
        std::unordered_map<std::string, std::unique_ptr<Operator>> dangling_productions;

        std::unique_ptr<Result> tree;
    };

    static void construct_tree(QueryContext& context, QueryPlan & plan);
    static void construct_scans(QueryContext& context, QueryPlan & plan);
    static void construct_selects(QueryContext & context, QueryPlan & plan);
    static void construct_projection(QueryContext & context, QueryPlan & plan);

};





