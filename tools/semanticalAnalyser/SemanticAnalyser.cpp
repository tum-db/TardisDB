#include "semanticAnalyser/SemanticAnalyser.hpp"

#include "foundations/Database.hpp"
#include "native/sql/SqlValues.hpp"
#include "foundations/version_management.hpp"
#include "semanticAnalyser/SemanticalVerifier.hpp"

#include <algorithm>
#include <cassert>
#include <stack>
#include <string>
#include <memory>
#include <native/sql/SqlTuple.hpp>

namespace semanticalAnalysis {

    void SemanticAnalyser::construct_scans(QueryContext& context, QueryPlan & plan, std::vector<Relation> &relations) {
        for (auto &relation : relations) {
            if (relation.alias.compare("") == 0) relation.alias = relation.name;

            // Recognize version
            std::string &branchName = relation.version;
            branch_id_t branchId;
            if (branchName.compare("master") != 0) {
                branchId = context.db._branchMapping[branchName];
            } else {
                branchId = master_branch_id;
            }

            //Construct the logical TableScan operator
            Table* table = context.db.getTable(relation.name);
            std::unique_ptr<TableScan> scan = std::make_unique<TableScan>(context, *table, branchId);

            //Store the ius produced by this TableScan
            for (iu_p_t iu : scan->getProduced()) {
                plan.ius[relation.alias][iu->columnInformation->columnName] = iu;
            }

            //Add a new production with TableScan as root node
            plan.dangling_productions.insert({relation.alias, std::move(scan)});
        }
    }

    // TODO: Implement nullable
    void SemanticAnalyser::construct_selects(QueryPlan& plan, std::vector<std::pair<Column,std::string>> &selections) {
        for (auto &[column,valueString] : selections) {
            // Construct Expression
            iu_p_t iu;
            if (column.table.compare("") == 0) {
                for (auto &[productionName,production] : plan.ius) {
                    for (auto &[key,value] : production) {
                        if (key.compare(column.name) == 0) {
                            column.table = productionName;
                            iu = value;
                        }
                    }
                }
            } else {
                iu = plan.ius[column.table][column.name];
            }
            auto constExp = std::make_unique<Expressions::Constant>(valueString, iu->columnInformation->type);
            auto identifier = std::make_unique<Expressions::Identifier>(iu);
            std::unique_ptr<Expressions::Comparison> exp = std::make_unique<Expressions::Comparison>(
                    Expressions::ComparisonMode::eq,
                    std::move(identifier),
                    std::move(constExp)
            );

            //Construct the logical Select operator
            std::unique_ptr<Select> select = std::make_unique<Select>(std::move(plan.dangling_productions[column.table]), std::move(exp));

            //Update corresponding production by setting the Select operator as new root node
            plan.dangling_productions[column.table] = std::move(select);
        }
    }

    void SemanticAnalyser::construct_join_graph(QueryContext & context, QueryPlan & plan, SelectStatement *stmt) {
        JoinGraph &graph = plan.graph;

        // create and add vertices to join graph
        for (auto & rel : stmt->relations) {
            std::string &tableAlias = rel.alias;
            if (rel.alias.length() == 0) tableAlias = rel.name;
            JoinGraph::Vertex v = JoinGraph::Vertex(plan.dangling_productions[tableAlias]);
            graph.addVertex(tableAlias,v);
        }

        // create edges
        for (auto &[vNode,uNode] : stmt->joinConditions) {
            //If edge does not already exist add it
            if (!graph.hasEdge(vNode.table,uNode.table)) {
                std::vector<Expressions::exp_op_t> expressions;
                JoinGraph::Edge edge = JoinGraph::Edge(vNode.table,uNode.table,expressions);
                graph.addEdge(edge);
            }

            //Get InformationUnits for both attributes
            iu_p_t iuV = plan.ius[vNode.table][vNode.name];
            iu_p_t iuU = plan.ius[uNode.table][uNode.name];

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

    void SemanticAnalyser::construct_join(std::string &vertexName, QueryContext &context, QueryPlan &plan) {
        // Get the vertex struct from the join graph
        JoinGraph::Vertex *vertex = plan.graph.getVertex(vertexName);

        // Get edges connected to the vertex
        std::vector<JoinGraph::Edge*> edges(0);
        plan.graph.getConnectedEdges(vertexName,edges);

        // Mark vertex as visited
        vertex->visited = true;

        // If the vertex is the first join component add it as new root node of the join graph
        if (plan.joinedTree == nullptr) {
            plan.joinedTree = std::move(vertex->production);
        }

        for (auto& edge : edges) {

            // Get struct of neighboring vertex
            std::string &neighboringVertexName = edge->uID;
            if (vertexName.compare(edge->vID) != 0) {
                neighboringVertexName = edge->vID;
            }
            JoinGraph::Vertex *neighboringVertex = plan.graph.getVertex(neighboringVertexName);

            // If neighboring vertex has already been visited => discard edge
            if (neighboringVertex->visited) continue;

            // If the edge is directed from the neighboring node to the current node also change the order of the join leafs
            if (vertexName.compare(edge->vID) != 0) {
                plan.joinedTree = std::make_unique<Join>(
                        std::move(neighboringVertex->production),
                        std::move(plan.joinedTree),
                        std::move(edge->expressions),
                        Join::Method::Hash
                );
            } else {
                plan.joinedTree = std::make_unique<Join>(
                        std::move(plan.joinedTree),
                        std::move(neighboringVertex->production),
                        std::move(edge->expressions),
                        Join::Method::Hash
                );
            }

            //Construct join for the neighboring node
            construct_join(neighboringVertexName, context, plan);
        }
    }

    void SemanticAnalyser::construct_joins(QueryContext & context, QueryPlan & plan, SQLParserResult &parserResult) {
        // Construct the join graph
        construct_join_graph(context,plan,parserResult.selectStmt);

        //Start with the first vertex in the vector of vertices of the join graph
        std::string firstVertexName = plan.graph.getFirstVertexName();

        //Construct join the first vertex
        construct_join(firstVertexName, context, plan);
    }

    std::unique_ptr<SemanticAnalyser> SemanticAnalyser::getSemanticAnalyser(QueryContext &context,
                                                                            SQLParserResult &parserResult) {
        switch (parserResult.opType) {
            case SQLParserResult::OpType::Select:
                return std::make_unique<SelectAnalyser>(context,parserResult);
            case SQLParserResult::OpType::Insert:
                return std::make_unique<InsertAnalyser>(context,parserResult);
            case SQLParserResult::OpType::Update:
                return std::make_unique<UpdateAnalyser>(context,parserResult);
            case SQLParserResult::OpType::Delete:
                return std::make_unique<DeleteAnalyser>(context,parserResult);
            case SQLParserResult::OpType::CreateTable:
                return std::make_unique<CreateTableAnalyser>(context,parserResult);
            case SQLParserResult::OpType::CreateBranch:
                return std::make_unique<CreateBranchAnalyser>(context,parserResult);
        }

        return nullptr;
    }
}



