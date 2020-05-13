#include "semanticAnalysis/SemanticAnalyser.hpp"

#include "foundations/Database.hpp"
#include "query_compiler/queryparser.hpp"

#include <algorithm>
#include <cassert>
#include <stack>
#include <string>
#include <memory>

void SemanticAnalyser::construct_scans(QueryContext& context, QueryPlan & plan) {
    auto& db = context.db;
    for (auto& relation : plan.parser_result.relations) {
        std::string tableName = relation.first;
        std::string tableAlias = relation.second;

        Table* table = db.getTable(tableName);

        //Construct the logical TableScan operator
        std::unique_ptr<TableScan> scan = std::make_unique<TableScan>(context, *table);

        //Store the ius produced by this TableScan
        const iu_set_t& produced = scan->getProduced();
        for (iu_p_t iu : produced) {
            plan.ius[iu->columnInformation->columnName] = iu;
            plan.iuNameToTable[iu->columnInformation->columnName] = table;
        }

        //Add a new production with TableScan as root node
        plan.dangling_productions.insert({tableAlias, std::move(scan)});
    }
}

void SemanticAnalyser::construct_selects(QueryContext& context, QueryPlan& plan) {
    auto & db = context.db;
    for (auto & selection : plan.parser_result.selections) {
        std::string& productionName = selection.first.first;
        std::string& productionIUName = selection.first.second;
        std::string& valueString = selection.second;

        iu_p_t iu = plan.ius[productionIUName];

        if (iu->columnInformation->type.nullable) {
            throw NotImplementedException();
        }

        //Construct Expression
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

void SemanticAnalyser::construct_join_graph(QueryContext & context, QueryPlan & plan) {
    JoinGraph &graph = plan.graph;

    // create and add vertices to join graph
    for (auto & rel : plan.parser_result.relations) {
        JoinGraph::Vertex v = JoinGraph::Vertex(plan.dangling_productions[rel.second]);
        graph.addVertex(rel.second,v);
    }

    // create edges
    for (auto & join_condition : plan.parser_result.joinConditions) {
        std::string &vName = join_condition.first.first;
        std::string &uName = join_condition.second.first;
        std::string &vColumn = join_condition.first.second;
        std::string &uColumn = join_condition.second.second;

        //If edge does not already exist add it
        if (!graph.hasEdge(vName,uName)) {
            std::vector<Expressions::exp_op_t> expressions;
            JoinGraph::Edge edge = JoinGraph::Edge(vName,uName,expressions);
            graph.addEdge(edge);
        }

        //Get InformationUnits for both attributes
        iu_p_t iuV = plan.ius[vColumn];
        iu_p_t iuU = plan.ius[uColumn];

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

void SemanticAnalyser::construct_joins(QueryContext & context, QueryPlan & plan) {
    //Start with the first vertex in the vector of vertices of the join graph
    std::string firstVertexName = plan.graph.getFirstVertexName();

    //Construct join the first vertex
    construct_join(firstVertexName, context, plan);
}

//TODO: Make projections available to every node in the tree
void SemanticAnalyser::construct_projection(QueryContext& context, QueryPlan & plan) {
    auto & db = context.db;

    //get projected IUs
    std::vector<iu_p_t> projectedIUs;
    for (const std::string& projectedIUName : plan.parser_result.projections) {
        if (plan.iuNameToTable.find(projectedIUName) == plan.iuNameToTable.end()) {
            throw std::runtime_error("column " + projectedIUName + " not in scope");
        }
        projectedIUs.push_back( plan.ius[projectedIUName] );
    }

    if (plan.joinedTree != nullptr) {
        //Construct Result and store it in the query plan struct
        plan.tree = std::make_unique<Result>( std::move(plan.joinedTree), projectedIUs );
    } else {
        throw std::runtime_error("no or more than one root found: Table joining has failed");
    }
}

void SemanticAnalyser::construct_tree(QueryContext& context, QueryPlan& plan) {
    construct_scans(context, plan);
    construct_selects(context, plan);
    construct_join_graph(context,plan);
    construct_joins(context,plan);
    construct_projection(context, plan);
}


std::unique_ptr<Result> SemanticAnalyser::parse_and_construct_tree(QueryContext& context, std::string sql) {
    QueryPlan plan;
    plan.parser_result = parse_and_analyse_sql_statement(context.db, sql);

    construct_tree(context, plan);

    return std::move(plan.tree);
}
