#include "semanticAnalysis/JoinGraph.hpp"

void JoinGraph::addVertex(std::string &alias, JoinGraph::Vertex &vertex) {
    vertices.insert({std::move(alias),std::move(vertex)});
}

void JoinGraph::addEdge(JoinGraph::Edge &edge) {
    edges.emplace_back(std::move(edge));
}

