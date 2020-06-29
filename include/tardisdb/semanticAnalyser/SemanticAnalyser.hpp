#include <string>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "algebra/logical/operators.hpp"
#include "foundations/Database.hpp"
#include "include/tardisdb/sqlParser/SQLParser.hpp"
#include "include/tardisdb/semanticAnalyser/JoinGraph.hpp"
#include "semanticAnalyser/SemanticalVerifier.hpp"

using namespace Algebra::Logical;

namespace semanticalAnalysis {
    struct QueryPlan {
        tardisParser::SQLParserResult parser_result;

        JoinGraph graph;

        std::unordered_map<std::string,std::unordered_map<std::string, iu_p_t>> ius;
        std::unordered_map<std::string, Table *> iuNameToTable;
        std::unordered_map<std::string, std::unique_ptr<Operator>> dangling_productions;

        std::unique_ptr<Operator> joinedTree;
        std::unique_ptr<Operator> tree;
    };

    class SemanticAnalyser {
    public:
        SemanticAnalyser(QueryContext &context, tardisParser::SQLParserResult &parserResult) : _context(context), _parserResult(parserResult) { _verifier = std::make_unique<SemanticalVerifier>(context); }
        virtual ~SemanticAnalyser() {};

        void verify() { _verifier->analyse_sql_statement(_parserResult); }
        virtual std::unique_ptr<Operator> constructTree() = 0;

    protected:
        QueryContext &_context;
        tardisParser::SQLParserResult &_parserResult;
        std::unique_ptr<SemanticalVerifier> _verifier;

    public:
        static std::unique_ptr<SemanticAnalyser> getSemanticAnalyser(QueryContext &context, tardisParser::SQLParserResult &parserResult);

    protected:
        static void construct_scans(QueryContext& context, QueryPlan & plan);
        static void construct_selects(QueryContext & context, QueryPlan & plan);
        static void construct_joins(QueryContext & context, QueryPlan & plan);
        static void construct_projection(QueryContext & context, QueryPlan & plan);
        static void construct_update(QueryContext & context, QueryPlan & plan);
        static void construct_delete(QueryContext & context, QueryPlan & plan);

    private:
        static void construct_join_graph(QueryContext & context, QueryPlan & plan);
        static void construct_join(std::string &vertexName, QueryContext &context, QueryPlan &plan);
    };

    //
    // StatementTypeAnalyser
    //

    class SelectAnalyser : public SemanticAnalyser {
    public:
        SelectAnalyser(QueryContext &context, tardisParser::SQLParserResult &parserResult) : SemanticAnalyser(context,parserResult) {}
        std::unique_ptr<Operator> constructTree() override;
    };

    class InsertAnalyser : public SemanticAnalyser {
    public:
        InsertAnalyser(QueryContext &context, tardisParser::SQLParserResult &parserResult) : SemanticAnalyser(context,parserResult) {}
        std::unique_ptr<Operator> constructTree() override;
    };

    class UpdateAnalyser : public SemanticAnalyser {
    public:
        UpdateAnalyser(QueryContext &context, tardisParser::SQLParserResult &parserResult) : SemanticAnalyser(context,parserResult) {}
        std::unique_ptr<Operator> constructTree() override;
    };

    class DeleteAnalyser : public SemanticAnalyser {
    public:
        DeleteAnalyser(QueryContext &context, tardisParser::SQLParserResult &parserResult) : SemanticAnalyser(context,parserResult) {}
        std::unique_ptr<Operator> constructTree() override;
    };

    class CreateTableAnalyser : public SemanticAnalyser {
    public:
        CreateTableAnalyser(QueryContext &context, tardisParser::SQLParserResult &parserResult) : SemanticAnalyser(context,parserResult) {}
        std::unique_ptr<Operator> constructTree() override;
    };

    class CreateBranchAnalyser : public SemanticAnalyser {
    public:
        CreateBranchAnalyser(QueryContext &context, tardisParser::SQLParserResult &parserResult) : SemanticAnalyser(context,parserResult) {}
        std::unique_ptr<Operator> constructTree() override;
    };
}







