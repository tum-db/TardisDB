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

    //TODO: Store version in binding
    void SemanticAnalyser::construct_scans(QueryContext& context, QueryPlan & plan, tardisParser::SQLParserResult parserResult) {
        if (parserResult.versions.size() != parserResult.relations.size()) {
            return;
        }

        size_t i = 0;
        for (auto &[tableName,tableAlias] : parserResult.relations) {
            if (parserResult.relations.size() == 1 && tableAlias.length() == 0) tableAlias = tableName;


            // Recognize version
            std::string &branchName = parserResult.versions[i];
            if (branchName.compare("master") != 0) {
                context.executionContext.branchIds.insert({tableAlias,context.db._branchMapping[branchName]});
            } else {
                context.executionContext.branchIds.insert({tableAlias,master_branch_id});
            }

            //Construct the logical TableScan operator
            Table* table = context.db.getTable(tableName);
            std::unique_ptr<TableScan> scan = std::make_unique<TableScan>(context, *table, new std::string(tableAlias));

            //Store the ius produced by this TableScan
            for (iu_p_t iu : scan->getProduced()) {
                plan.ius[tableAlias][iu->columnInformation->columnName] = iu;
            }

            //Add a new production with TableScan as root node
            plan.dangling_productions.insert({tableAlias, std::move(scan)});
            i++;
        }
    }

    // TODO: Implement nullable
    void SemanticAnalyser::construct_selects(QueryContext& context, QueryPlan& plan, tardisParser::SQLParserResult parserResult) {
        for (auto &[selection,valueString] : parserResult.selections) {
            auto &[productionName,productionIUName] = selection;
            if (parserResult.relations.size() == 1 && productionName.length() == 0) productionName = parserResult.relations[0].first;

            // Construct Expression
            iu_p_t iu = plan.ius[productionName][productionIUName];
            auto constExp = std::make_unique<Expressions::Constant>(valueString, iu->columnInformation->type);
            auto identifier = std::make_unique<Expressions::Identifier>(iu);
            std::unique_ptr<Expressions::Comparison> exp = std::make_unique<Expressions::Comparison>(
                    Expressions::ComparisonMode::eq,
                    std::move(identifier),
                    std::move(constExp)
            );

            //Construct the logical Select operator
            std::unique_ptr<Select> select = std::make_unique<Select>(std::move(plan.dangling_productions[productionName]), std::move(exp));

            //Update corresponding production by setting the Select operator as new root node
            plan.dangling_productions[productionName] = std::move(select);
        }
    }

    void SemanticAnalyser::construct_join_graph(QueryContext & context, QueryPlan & plan, tardisParser::SQLParserResult parserResult) {
        JoinGraph &graph = plan.graph;

        // create and add vertices to join graph
        for (auto & rel : parserResult.relations) {
            std::string &tableAlias = rel.second;
            if (rel.second.length() == 0) tableAlias = rel.first;
            JoinGraph::Vertex v = JoinGraph::Vertex(plan.dangling_productions[tableAlias]);
            graph.addVertex(tableAlias,v);
        }

        // create edges
        for (auto &[vNode,uNode] : parserResult.joinConditions) {
            auto &[vName,vColumn] = vNode;
            auto &[uName,uColumn] = uNode;

            //If edge does not already exist add it
            if (!graph.hasEdge(vName,uName)) {
                std::vector<Expressions::exp_op_t> expressions;
                JoinGraph::Edge edge = JoinGraph::Edge(vName,uName,expressions);
                graph.addEdge(edge);
            }

            //Get InformationUnits for both attributes
            iu_p_t iuV = plan.ius[vName][vColumn];
            iu_p_t iuU = plan.ius[uName][uColumn];

            //Create new compare expression as join condition
            std::vector<Expressions::exp_op_t> joinExprVec;
            auto joinExpr = std::make_unique<Expressions::Comparison>(
                    Expressions::ComparisonMode::eq, // equijoin
                    std::make_unique<Expressions::Identifier>(iuV),
                    std::make_unique<Expressions::Identifier>(iuU)
            );
            //Add join condition to edge
            graph.getEdge(vName,uName)->expressions.push_back(std::move(joinExpr));
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

    void SemanticAnalyser::construct_joins(QueryContext & context, QueryPlan & plan, tardisParser::SQLParserResult parserResult) {
        // Construct the join graph
        construct_join_graph(context,plan,parserResult);

        //Start with the first vertex in the vector of vertices of the join graph
        std::string firstVertexName = plan.graph.getFirstVertexName();

        //Construct join the first vertex
        construct_join(firstVertexName, context, plan);
    }

    std::unique_ptr<SemanticAnalyser> SemanticAnalyser::getSemanticAnalyser(QueryContext &context,
            tardisParser::SQLParserResult &parserResult) {
        switch (parserResult.opType) {
            case tardisParser::SQLParserResult::OpType::Select:
                return std::make_unique<SelectAnalyser>(context,parserResult);
            case tardisParser::SQLParserResult::OpType::Insert:
                return std::make_unique<InsertAnalyser>(context,parserResult);
            case tardisParser::SQLParserResult::OpType::Update:
                return std::make_unique<UpdateAnalyser>(context,parserResult);
            case tardisParser::SQLParserResult::OpType::Delete:
                return std::make_unique<DeleteAnalyser>(context,parserResult);
            case tardisParser::SQLParserResult::OpType::CreateTable:
                return std::make_unique<CreateTableAnalyser>(context,parserResult);
            case tardisParser::SQLParserResult::OpType::CreateBranch:
                return std::make_unique<CreateBranchAnalyser>(context,parserResult);
        }

        return nullptr;
    }
}



