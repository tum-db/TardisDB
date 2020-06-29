#include <string>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "algebra/logical/operators.hpp"
#include "foundations/Database.hpp"
#include "include/tardisdb/sqlParser/SQLParser.hpp"
#include "include/tardisdb/semanticAnalyser/JoinGraph.hpp"

using namespace Algebra::Logical;

class SemanticAnalyser {
public:
    static std::unique_ptr<Operator> parse_and_construct_tree(QueryContext& context, std::string sql);

private:
    struct QueryPlan {
        tardisParser::SQLParserResult parser_result;

        JoinGraph graph;

        std::unordered_map<std::string,std::unordered_map<std::string, iu_p_t>> ius;
        std::unordered_map<std::string, Table *> iuNameToTable;
        std::unordered_map<std::string, std::unique_ptr<Operator>> dangling_productions;

        std::unique_ptr<Operator> joinedTree;
        std::unique_ptr<Operator> tree;
    };

    static void constructSelect(QueryContext& context, QueryPlan & plan);
    static void constructInsert(QueryContext &context, QueryPlan &plan);
    static void constructUpdate(QueryContext& context, QueryPlan & plan);
    static void constructDelete(QueryContext& context, QueryPlan & plan);
    static void constructCreate(QueryContext& context, QueryPlan & plan);
    static void constructCheckout(QueryContext& context, QueryPlan & plan);

    static void construct_scans(QueryContext& context, QueryPlan & plan);
    static void construct_selects(QueryContext & context, QueryPlan & plan);
    static void construct_join_graph(QueryContext & context, QueryPlan & plan);
    static void construct_join(std::string &vertexName, QueryContext &context, QueryPlan &plan);
    static void construct_joins(QueryContext & context, QueryPlan & plan);
    static void construct_projection(QueryContext & context, QueryPlan & plan);
    static void construct_update(QueryContext & context, QueryPlan & plan);
    static void construct_delete(QueryContext & context, QueryPlan & plan);

    static void analyse_sql_statement(Database& db, tardisParser::SQLParserResult &result);
};





