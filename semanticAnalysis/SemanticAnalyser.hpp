#include "foundations/Database.hpp"
#include "semanticAnalysis/SQLParser.hpp"
#include "algebra/logical/operators.hpp"
#include <string>
#include <memory>
#include <unordered_map>
#include <unordered_set>

using namespace Algebra::Logical;

class JoinGraph {
public:
    struct Vertex {
        std::unique_ptr<Operator> production;
        bool visited = false;

        Vertex(std::unique_ptr<Operator>& production) : production(std::move(production)) { }
    };

    struct Edge {
        std::string vID, uID;
        std::vector<Expressions::exp_op_t> expressions;

        Edge(std::string &vID, std::string &uID, std::vector<Expressions::exp_op_t>& expressions) : vID(vID), uID(uID), expressions(std::move(expressions)) { }
    };

    void addVertex(std::string &alias, Vertex& vertex);
    void addEdge(Edge& edge);
    bool hasEdge(std::string &vAlias, std::string &uAlias) {
        for (auto& edge : edges) {
            if ((edge.vID.compare(vAlias) == 0 && edge.uID.compare(uAlias) == 0) || (edge.vID.compare(uAlias) == 0 && edge.uID.compare(vAlias) == 0)) {
                return true;
            }
        }
        return false;
    }
    Vertex* getVertex(std::string &vName) {
        return &(vertices.at(vName));
    }
    std::string getFirstVertexName() {
        return vertices.begin()->first;
    }
    Edge* getEdge(std::string &vAlias, std::string &uAlias) {
        for (auto& edge : edges) {
            if ((edge.vID.compare(vAlias) == 0 && edge.uID.compare(uAlias) == 0) || (edge.vID.compare(uAlias) == 0 && edge.uID.compare(vAlias) == 0)) {
                return &edge;
            }
        }
        return nullptr;
    }
    void getConnectedEdges(std::string &vAlias, std::vector<Edge*> connectedEdges) {
        for (auto& edge : edges) {
            if (edge.vID.compare(vAlias) == 0 || edge.uID.compare(vAlias) == 0) {
                connectedEdges.emplace_back(&edge);
            }
        }
    }
private:
    std::unordered_map<std::string, Vertex> vertices;
    std::vector<Edge> edges;
};

class SemanticAnalyser {
public:
    static std::unique_ptr<Result> parse_and_construct_tree(QueryContext& context, std::string sql);

private:
    struct QueryPlan {
        SQLParserResult parser_result;

        JoinGraph graph;

        std::unordered_map<std::string, iu_p_t> ius;
        std::unordered_map<std::string, Table *> iuNameToTable;
        std::unordered_map<std::string, std::unique_ptr<Operator>> dangling_productions;

        std::unique_ptr<Operator> joinedTree;
        std::unique_ptr<Result> tree;
    };

    static void construct_tree(QueryContext& context, QueryPlan & plan);
    static void construct_scans(QueryContext& context, QueryPlan & plan);
    static void construct_selects(QueryContext & context, QueryPlan & plan);
    static void construct_join_graph(QueryContext & context, QueryPlan & plan);
    static void construct_join(std::string &vertexName, QueryContext &context, QueryPlan &plan);
    static void construct_joins(QueryContext & context, QueryPlan & plan);
    static void construct_projection(QueryContext & context, QueryPlan & plan);

};





