#include <string>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "algebra/logical/operators.hpp"
#include "foundations/Database.hpp"
#include "include/tardisdb/sqlParser/SQLParser.hpp"
#include "include/tardisdb/semanticAnalyser/JoinGraph.hpp"

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
        SemanticAnalyser(QueryContext &context) : _context(context) {}
        virtual ~SemanticAnalyser() {};

        virtual bool verify() = 0;
        virtual void constructTree(QueryPlan & plan) = 0;

    protected:
        QueryContext &_context;

        static void construct_scans(QueryContext& context, QueryPlan & plan);
        static void construct_selects(QueryContext & context, QueryPlan & plan);
        static void construct_joins(QueryContext & context, QueryPlan & plan);
        static void construct_projection(QueryContext & context, QueryPlan & plan);
        static void construct_update(QueryContext & context, QueryPlan & plan);
        static void construct_delete(QueryContext & context, QueryPlan & plan);

    private:
        static void construct_join_graph(QueryContext & context, QueryPlan & plan);
        static void construct_join(std::string &vertexName, QueryContext &context, QueryPlan &plan);

    public:
        static std::unique_ptr<Operator> parse_and_construct_tree(QueryContext& context, std::string sql);
    };

    //
    // StatementTypeAnalyser
    //

    class SelectAnalyser : public SemanticAnalyser {
    public:
        SelectAnalyser(QueryContext &context) : SemanticAnalyser(context) {}
        bool verify() override { return true; }
        void constructTree(QueryPlan & plan) override;
    };

    class InsertAnalyser : public SemanticAnalyser {
    public:
        InsertAnalyser(QueryContext &context) : SemanticAnalyser(context) {}
        bool verify() override { return true; }
        void constructTree(QueryPlan & plan) override;
    };

    class UpdateAnalyser : public SemanticAnalyser {
    public:
        UpdateAnalyser(QueryContext &context) : SemanticAnalyser(context) {}
        bool verify() override { return true; }
        void constructTree(QueryPlan & plan) override;
    };

    class DeleteAnalyser : public SemanticAnalyser {
    public:
        DeleteAnalyser(QueryContext &context) : SemanticAnalyser(context) {}
        bool verify() override { return true; }
        void constructTree(QueryPlan & plan) override;
    };

    class CreateTableAnalyser : public SemanticAnalyser {
    public:
        CreateTableAnalyser(QueryContext &context) : SemanticAnalyser(context) {}
        bool verify() override { return true; }
        void constructTree(QueryPlan & plan) override;
    };

    class CreateBranchAnalyser : public SemanticAnalyser {
    public:
        CreateBranchAnalyser(QueryContext &context) : SemanticAnalyser(context) {}
        bool verify() override { return true; }
        void constructTree(QueryPlan & plan) override;
    };
}







