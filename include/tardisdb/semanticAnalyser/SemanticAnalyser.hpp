#include <string>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "algebra/logical/operators.hpp"
#include "foundations/Database.hpp"
#include "semanticAnalyser/AnalyzingContext.hpp"
#include "semanticAnalyser/ParserResult.hpp"
#include "semanticAnalyser/JoinGraph.hpp"

using namespace Algebra::Logical;

namespace semanticalAnalysis {
    struct semantic_sql_error : std::runtime_error {
        //semantic or syntactic errors
        using std::runtime_error::runtime_error;
    };

    class SemanticAnalyser {
    public:
        SemanticAnalyser(AnalyzingContext &context) : _context(context) {}
        virtual ~SemanticAnalyser() {};

        virtual void verify() = 0;
        virtual void constructTree() = 0;

    protected:
        AnalyzingContext &_context;

    public:
        static std::unique_ptr<SemanticAnalyser> getSemanticAnalyser(AnalyzingContext &context);

    protected:
        static void construct_scans(AnalyzingContext& context, Relation &relation) {
            std::vector<Relation> relations;
            relations.push_back(relation);
            construct_scans(context,relations);
        }
        static void construct_scans(AnalyzingContext& context, std::vector<Relation> &relations);
        static void construct_selects(AnalyzingContext& context, std::vector<std::pair<Column,std::string>> &selections);
    };

    //
    // StatementTypeAnalyser
    //

    class SelectAnalyser : public SemanticAnalyser {
    public:
        SelectAnalyser(AnalyzingContext &context) : SemanticAnalyser(context) {}
        void verify() override;
        void constructTree() override;
    private:
        static void construct_joins(AnalyzingContext & context);
        static void construct_join_graph(AnalyzingContext & context, SelectStatement *stmt);
        static void construct_join(AnalyzingContext &context, std::string &vertexName);
    };

    class InsertAnalyser : public SemanticAnalyser {
    public:
        InsertAnalyser(AnalyzingContext &context) : SemanticAnalyser(context) {}
        void verify() override;
        void constructTree() override;
    };

    class UpdateAnalyser : public SemanticAnalyser {
    public:
        UpdateAnalyser(AnalyzingContext &context) : SemanticAnalyser(context) {}
        void verify() override;
        void constructTree() override;
    };

    class DeleteAnalyser : public SemanticAnalyser {
    public:
        DeleteAnalyser(AnalyzingContext &context) : SemanticAnalyser(context) {}
        void verify() override;
        void constructTree() override;
    };

    class CreateTableAnalyser : public SemanticAnalyser {
    public:
        CreateTableAnalyser(AnalyzingContext &context) : SemanticAnalyser(context) {}
        void verify() override;
        void constructTree() override;
    };

    class CreateBranchAnalyser : public SemanticAnalyser {
    public:
        CreateBranchAnalyser(AnalyzingContext &context) : SemanticAnalyser(context) {}
        void verify() override;
        void constructTree() override;
    };

    class BranchAnalyser : public SemanticAnalyser {
    public:
        BranchAnalyser(AnalyzingContext &context) : SemanticAnalyser(context) {}
        void verify() override;
        void constructTree() override;
    };

    class CopyTableAnalyser : public SemanticAnalyser {
    public:
        CopyTableAnalyser(AnalyzingContext &context) : SemanticAnalyser(context) {}
        void verify() override;
        void constructTree() override;
    };

    class DumpTableAnalyser : public SemanticAnalyser {
    public:
        DumpTableAnalyser(AnalyzingContext &context) : SemanticAnalyser(context) {}
        void verify() override;
        void constructTree() override;
    };
}







