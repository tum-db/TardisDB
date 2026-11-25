#include <functional>
#include <map>
#include <set>
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

    /// An aggregate query that the aggregation over branches does not compute (aggregates of expressions,
    /// in expressions, over several tables, of arrays or text, GROUP BY columns): QueryCompiler computes it
    /// from the rows of a query (see SelectAnalyser::needsGeneralAggregation)
    struct general_aggregation_required : semantic_sql_error {
        general_aggregation_required() : semantic_sql_error("the query needs the general aggregation") { }
    };

    /// \returns Whether an expression of the select list contains an aggregate (count, sum, avg, min, max)
    bool containsAggregate(const sqlExpressions::Node & n);

    /// A NULL, or a value that may be NULL, for a column declared NOT NULL (SQLSTATE 23502)
    struct not_null_violation : semantic_sql_error {
        using semantic_sql_error::semantic_sql_error;
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
        /// A Select per predicate (ranges, row values, IN, <>) over the production of its table
        static void construct_predicates(AnalyzingContext& context, std::vector<Predicate> &predicates);
        /// The condition of a predicate (IS [NOT] NULL, IN, <>, ranges of columns or of row values) over the attributes of its columns
        static Expressions::exp_op_t predicateExpression(const Predicate & predicate, const std::vector<iu_p_t> & ius);
        /// The columns of the productions of the context; adds the productions they belong to
        static std::function<iu_p_t(const std::string &, const std::string &)> columnResolver(AnalyzingContext & context, std::set<std::string> * productions = nullptr);
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
        /// Aggregates over the rows of one branch that the aggregation over branches does not compute
        bool needsGeneralAggregation(SelectStatement *stmt);
        // branch-aware queries: aggregates per branch over a set of branches
        static bool isBranchQuery(SelectStatement *stmt);
        static std::vector<branch_id_t> resolveBranches(Database &db, Relation &relation);
        void verifyBranchQuery();
        void constructBranchQuery();

        // SELECT ... FROM DIFF ( t VERSION a , t VERSION b )
        static bool isDiffQuery(SelectStatement *stmt);
        std::vector<std::string> diffColumnNames(Table &table, Relation &relation);
        void verifyDiffQuery();
        void constructDiffQuery();

        static void construct_joins(AnalyzingContext & context);
        static void construct_join_graph(AnalyzingContext & context, SelectStatement *stmt);
        static void construct_join(AnalyzingContext &context, std::string &vertexName);
        static void addJoinCondition(JoinGraph & graph, std::string leftTable, std::string rightTable, Expressions::exp_op_t condition);
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
        std::string returnJSON();
    };

    class CreateIndexAnalyser : public SemanticAnalyser {
    public:
        CreateIndexAnalyser(AnalyzingContext &context) : SemanticAnalyser(context) {}
        void verify() override;
        void constructTree() override;
    };

    class DropBranchAnalyser : public SemanticAnalyser {
    public:
        DropBranchAnalyser(AnalyzingContext &context) : SemanticAnalyser(context) {}
        void verify() override;
        void constructTree() override;
    };

    class MergeBranchAnalyser : public SemanticAnalyser {
    public:
        MergeBranchAnalyser(AnalyzingContext &context) : SemanticAnalyser(context) {}
        void verify() override;
        void constructTree() override;
    };

    class AlterTableAnalyser : public SemanticAnalyser {
    public:
        AlterTableAnalyser(AnalyzingContext &context) : SemanticAnalyser(context) {}
        void verify() override;
        void constructTree() override;
    };

    /// The attribute of a column (binding may be empty), or nullptr
    using ColumnResolver = std::function<iu_p_t(const std::string & table, const std::string & name)>;

    /// The logical expression of a parsed expression (of the select list, WHERE or SET): the types of
    /// constants from their context, casts between numbers, the functions and operators of arrays
    class ExpressionAnalyser {
    public:
        explicit ExpressionAnalyser(ColumnResolver resolve);

        Expressions::exp_op_t analyse(const sqlExpressions::Node & n, const Sql::SqlType * expected = nullptr);

        /// a condition of WHERE (boolean)
        Expressions::exp_op_t analyseCondition(const sqlExpressions::Node & n);

        /// the new value of a column of SET
        Expressions::exp_op_t analyseValue(const sqlExpressions::Node & n, Sql::SqlType target, const std::string & column);

    private:
        ColumnResolver _resolve;

        Expressions::exp_op_t constant(const sqlExpressions::Node & n, Sql::SqlType type);
        Expressions::exp_op_t coerce(Expressions::exp_op_t e, Sql::SqlType target, const std::string & what);
        std::pair<Expressions::exp_op_t, Expressions::exp_op_t> typedPair(const sqlExpressions::Node & l, const sqlExpressions::Node & r, const std::string & what);
        Expressions::exp_op_t arrayConstructor(const sqlExpressions::Node & n, const Sql::SqlType * expected);
        Expressions::exp_op_t integerArgument(const sqlExpressions::Node & n, const std::string & what);
        Expressions::exp_op_t textArgument(const sqlExpressions::Node & n, const std::string & what);
        Expressions::exp_op_t subscript(const sqlExpressions::Node & n);
        Expressions::exp_op_t polymorphic(const std::string & name, const std::vector<const sqlExpressions::Node *> & arrays,
                                          const std::vector<const sqlExpressions::Node *> & elements, std::vector<Expressions::exp_op_t> & arrayOut,
                                          std::vector<Expressions::exp_op_t> & elementOut, Sql::SqlType & arrayType);
        Expressions::exp_op_t function(const sqlExpressions::Node & n);
        Expressions::exp_op_t operation(const sqlExpressions::Node & n);
        // linear algebra on arrays of numbers
        Expressions::exp_op_t numericArgument(const sqlExpressions::Node & n, const std::string & what);
        Expressions::exp_op_t arrayArithmetic(const sqlExpressions::Node & n);
        Expressions::exp_op_t linearAlgebraFunction(const sqlExpressions::Node & n);
    };

    /// \returns Whether CREATE TABLE and ALTER TABLE know the type
    bool isColumnType(const std::string &type);

    /// \returns The type of a column of CREATE TABLE or ALTER TABLE
    Sql::SqlType toColumnType(const ColumnSpec &columnSpec);

    class CopyTableAnalyser : public SemanticAnalyser {
    public:
        CopyTableAnalyser(AnalyzingContext &context) : SemanticAnalyser(context) {}
        void verify() override;
        void constructTree() override;
    };
}







