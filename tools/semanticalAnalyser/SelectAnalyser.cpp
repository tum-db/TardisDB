//
// Created by Blum Thomas on 2020-06-29.
//

#include "semanticAnalyser/SemanticAnalyser.hpp"

namespace semanticalAnalysis {

    void SelectAnalyser::verify() {
        Database &db = _context.db;
        SelectStatement* stmt = _context.parserResult.selectStmt;
        if (stmt == nullptr) throw semantic_sql_error("unknown statement type");

        std::set<std::string> unbindedColumns;
        std::set<std::string> relations;
        std::unordered_map<std::string,std::vector<std::string>> bindedColumns;
        for (auto &relation : stmt->relations) {
            if (!db.hasTable(relation.name)) throw semantic_sql_error("table '" + relation.name + "' does not exist");
            if (db._branchMapping.find(relation.version) == db._branchMapping.end()) throw semantic_sql_error("version '" + relation.version + "' does not exist");
            if (relation.alias.compare("") == 0) {
                if (relations.find(relation.name) != relations.end()) throw semantic_sql_error("relation '" + relation.name + "' is already specified - use bindings!");
            }
            relations.insert(relation.name);
            Table *table = db.getTable(relation.name);
            std::vector<std::string> columnNames = table->getColumnNames();
            std::vector<std::string> intersectResult;
            std::set_intersection(unbindedColumns.begin(),unbindedColumns.end(),columnNames.begin(),columnNames.end(),std::back_inserter(intersectResult));
            if (!intersectResult.empty()) throw semantic_sql_error("columns are ambiguous - use bindings!");
            if (relation.alias.compare("") == 0) {
                unbindedColumns.insert(columnNames.begin(),columnNames.end());
            } else {
                if (bindedColumns.find(relation.alias) != bindedColumns.end()) throw semantic_sql_error("binding '" + relation.alias + "' is already specified - use bindings!");
                bindedColumns[relation.alias] = columnNames;
            }
        }

        for (auto &column : stmt->selections) {
            if (column.first.table.compare("") == 0) {
                if (unbindedColumns.find(column.first.name) == unbindedColumns.end())
                    throw semantic_sql_error("column '" + column.first.name + "' is not specified");
            } else {
                if (bindedColumns.find(column.first.table) == bindedColumns.end())
                    throw semantic_sql_error("binding '" + column.first.table + "' is not specified");
                if (std::find(bindedColumns[column.first.table].begin(),bindedColumns[column.first.table].end(),column.first.name) == bindedColumns[column.first.table].end())
                    throw semantic_sql_error("column '" + column.first.name + "' is not specified");
            }
        }
        for (auto &column : stmt->projections) {
            if (column.table.compare("") == 0) {
                if (unbindedColumns.find(column.name) == unbindedColumns.end())
                    throw semantic_sql_error("column '" + column.name + "' is not specified");
            } else {
                if (bindedColumns.find(column.table) == bindedColumns.end())
                    throw semantic_sql_error("binding '" + column.table + "' is not specified");
                if (std::find(bindedColumns[column.table].begin(),bindedColumns[column.table].end(),column.name) == bindedColumns[column.table].end())
                    throw semantic_sql_error("column '" + column.name + "' is not specified");
            }
        }
        for (auto &column : stmt->joinConditions) {
            if (column.first.table.compare("") == 0) {
                if (unbindedColumns.find(column.first.name) == unbindedColumns.end())
                    throw semantic_sql_error("column '" + column.first.name + "' is not specified");
            } else {
                if (bindedColumns.find(column.first.table) == bindedColumns.end())
                    throw semantic_sql_error("binding '" + column.first.table + "' is not specified");
                if (std::find(bindedColumns[column.first.table].begin(),bindedColumns[column.first.table].end(),column.first.name) == bindedColumns[column.first.table].end())
                    throw semantic_sql_error("column '" + column.first.name + "' is not specified");
            }
            if (column.second.table.compare("") == 0) {
                if (unbindedColumns.find(column.second.name) == unbindedColumns.end())
                    throw semantic_sql_error("column '" + column.second.name + "' is not specified");
            } else {
                if (bindedColumns.find(column.second.table) == bindedColumns.end())
                    throw semantic_sql_error("binding '" + column.second.table + "' is not specified");
                if (std::find(bindedColumns[column.second.table].begin(),bindedColumns[column.second.table].end(),column.second.name) == bindedColumns[column.second.table].end())
                    throw semantic_sql_error("column '" + column.second.name + "' is not specified");
            }
        }
        // For each relation
        // // Relation with name exists?
        // // Branch exists?
        // Check equal relations
        // Check equal columns
    }

    //TODO: Make projections available to every node in the tree
//TODO: Check physical tree projections do not work after joining
    void SelectAnalyser::constructTree() {
        SelectStatement *stmt = _context.parserResult.selectStmt;

        construct_scans(_context, stmt->relations);
        construct_selects(_context, stmt->selections);
        construct_joins(_context);

        auto & db = _context.db;

        //get projected IUs
        std::vector<iu_p_t> projectedIUs;
        for (auto &column : stmt->projections) {
            if (column.table.length() == 0) {
                for (auto& production : _context.ius) {
                    for (auto& iu : production.second) {
                        if (iu.first.compare(column.name) == 0) {
                            projectedIUs.push_back( iu.second );
                        }
                    }
                }
            } else {
                projectedIUs.push_back(_context.ius[column.table][column.name]);
            }
        }

        _context.joinedTree = std::make_unique<Result>( std::move(_context.joinedTree), projectedIUs );
    }

    void SelectAnalyser::construct_join_graph(AnalyzingContext & context, SelectStatement *stmt) {
        JoinGraph &graph = context.graph;

        // create and add vertices to join graph
        for (auto & rel : stmt->relations) {
            std::string &tableAlias = rel.alias;
            if (rel.alias.length() == 0) tableAlias = rel.name;
            JoinGraph::Vertex v = JoinGraph::Vertex(context.dangling_productions[tableAlias]);
            graph.addVertex(tableAlias,v);
        }

        // create edges
        for (auto &[vNode,uNode] : stmt->joinConditions) {
            if (vNode.table.length() == 0) {
                for (auto& production : context.ius) {
                    for (auto& iu : production.second) {
                        if (iu.first.compare(vNode.name) == 0) {
                            vNode.table = production.first;
                            break;
                        }
                    }
                }
            }
            if (uNode.table.length() == 0) {
                for (auto& production : context.ius) {
                    for (auto& iu : production.second) {
                        if (iu.first.compare(uNode.name) == 0) {
                            uNode.table = production.first;
                            break;
                        }
                    }
                }
            }

            //If edge does not already exist add it
            if (!graph.hasEdge(vNode.table,uNode.table)) {
                std::vector<Expressions::exp_op_t> expressions;
                JoinGraph::Edge edge = JoinGraph::Edge(vNode.table,uNode.table,expressions);
                graph.addEdge(edge);
            }

            //Get InformationUnits for both attributes
            iu_p_t iuV = context.ius[vNode.table][vNode.name];
            iu_p_t iuU = context.ius[uNode.table][uNode.name];

            //Create new compare expression as join condition
            std::vector<Expressions::exp_op_t> joinExprVec;
            auto joinExpr = std::make_unique<Expressions::Comparison>(
                    Expressions::ComparisonMode::eq, // equijoin
                    std::make_unique<Expressions::Identifier>(iuV),
                    std::make_unique<Expressions::Identifier>(iuU)
            );
            //Add join condition to edge
            graph.getEdge(vNode.table,uNode.table)->expressions.push_back(std::move(joinExpr));
        }
    }

    void SelectAnalyser::construct_join(AnalyzingContext &context, std::string &vertexName) {
        // Get the vertex struct from the join graph
        JoinGraph::Vertex *vertex = context.graph.getVertex(vertexName);

        // Get edges connected to the vertex
        std::vector<JoinGraph::Edge*> edges(0);
        context.graph.getConnectedEdges(vertexName,edges);

        // Mark vertex as visited
        vertex->visited = true;

        // If the vertex is the first join component add it as new root node of the join graph
        if (context.joinedTree == nullptr) {
            context.joinedTree = std::move(vertex->production);
        }

        for (auto& edge : edges) {

            // Get struct of neighboring vertex
            std::string &neighboringVertexName = edge->uID;
            if (vertexName.compare(edge->vID) != 0) {
                neighboringVertexName = edge->vID;
            }
            JoinGraph::Vertex *neighboringVertex = context.graph.getVertex(neighboringVertexName);

            // If neighboring vertex has already been visited => discard edge
            if (neighboringVertex->visited) continue;

            // If the edge is directed from the neighboring node to the current node also change the order of the join leafs
            if (vertexName.compare(edge->vID) != 0) {
                context.joinedTree = std::make_unique<Join>(
                        std::move(neighboringVertex->production),
                        std::move(context.joinedTree),
                        std::move(edge->expressions),
                        Join::Method::Hash
                );
            } else {
                context.joinedTree = std::make_unique<Join>(
                        std::move(context.joinedTree),
                        std::move(neighboringVertex->production),
                        std::move(edge->expressions),
                        Join::Method::Hash
                );
            }

            //Construct join for the neighboring node
            construct_join(context, neighboringVertexName);
        }
    }

    void SelectAnalyser::construct_joins(AnalyzingContext & context) {
        // Construct the join graph
        construct_join_graph(context,context.parserResult.selectStmt);

        //Start with the first vertex in the vector of vertices of the join graph
        std::string firstVertexName = context.graph.getFirstVertexName();

        //Construct join the first vertex
        construct_join(context, firstVertexName);

        if (context.joinedTree == nullptr)
            throw std::runtime_error("no or more than one root found: Table joining has failed");
    }

}