#include <string>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "algebra/logical/operators.hpp"
#include "foundations/Database.hpp"
#include "semanticAnalyser/ParserResult.hpp"
#include "include/tardisdb/semanticAnalyser/JoinGraph.hpp"
#include "semanticAnalyser/SemanticalVerifier.hpp"

using namespace Algebra::Logical;

namespace semanticalAnalysis {
    struct QueryPlan {
        JoinGraph graph;

        std::unordered_map<std::string,std::unordered_map<std::string, iu_p_t>> ius;
        std::unordered_map<std::string, std::unique_ptr<Operator>> dangling_productions;

        std::unique_ptr<Operator> joinedTree;
    };

    class SemanticAnalyser {
    public:
        SemanticAnalyser(QueryContext &context, SQLParserResult &parserResult) : _context(context), _parserResult(parserResult) { _verifier = std::make_unique<SemanticalVerifier>(context); }
        virtual ~SemanticAnalyser() {};

        void verify() { _verifier->analyse_sql_statement(_parserResult); }
        virtual std::unique_ptr<Operator> constructTree() = 0;

    protected:
        QueryContext &_context;
        SQLParserResult &_parserResult;
        std::unique_ptr<SemanticalVerifier> _verifier;

    public:
        static std::unique_ptr<SemanticAnalyser> getSemanticAnalyser(QueryContext &context, SQLParserResult &parserResult);

    protected:
        static void construct_scans(QueryContext& context, QueryPlan & plan, std::vector<Relation> &relations);
        static void construct_selects(QueryPlan & plan, std::vector<std::pair<Column,std::string>> &selections);
        static void construct_joins(QueryContext & context, QueryPlan & plan, SQLParserResult &parserResult);

    private:
        static void construct_join_graph(QueryContext & context, QueryPlan & plan, SelectStatement *stmt);
        static void construct_join(std::string &vertexName, QueryContext &context, QueryPlan &plan);
    };

    //
    // StatementTypeAnalyser
    //

    class SelectAnalyser : public SemanticAnalyser {
    public:
        SelectAnalyser(QueryContext &context, SQLParserResult &parserResult) : SemanticAnalyser(context,parserResult) {}
        std::unique_ptr<Operator> constructTree() override;
    };

    class InsertAnalyser : public SemanticAnalyser {
    public:
        InsertAnalyser(QueryContext &context, SQLParserResult &parserResult) : SemanticAnalyser(context,parserResult) {}
        std::unique_ptr<Operator> constructTree() override;
    };

    class UpdateAnalyser : public SemanticAnalyser {
    public:
        UpdateAnalyser(QueryContext &context, SQLParserResult &parserResult) : SemanticAnalyser(context,parserResult) {}
        std::unique_ptr<Operator> constructTree() override;
    };

    class DeleteAnalyser : public SemanticAnalyser {
    public:
        DeleteAnalyser(QueryContext &context, SQLParserResult &parserResult) : SemanticAnalyser(context,parserResult) {}
        std::unique_ptr<Operator> constructTree() override;
    };

    class CreateTableAnalyser : public SemanticAnalyser {
    public:
        CreateTableAnalyser(QueryContext &context, SQLParserResult &parserResult) : SemanticAnalyser(context,parserResult) {}
        std::unique_ptr<Operator> constructTree() override;
    };

    class CreateBranchAnalyser : public SemanticAnalyser {
    public:
        CreateBranchAnalyser(QueryContext &context, SQLParserResult &parserResult) : SemanticAnalyser(context,parserResult) {}
        std::unique_ptr<Operator> constructTree() override;
    };
}







