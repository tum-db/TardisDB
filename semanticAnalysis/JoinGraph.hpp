#ifndef PROTODB_JOINGRAPH_HPP
#define PROTODB_JOINGRAPH_HPP

#include "algebra/logical/operators.hpp"

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
    void getConnectedEdges(std::string &vAlias, std::vector<Edge*> &connectedEdges) {
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

#endif //PROTODB_JOINGRAPH_HPP
