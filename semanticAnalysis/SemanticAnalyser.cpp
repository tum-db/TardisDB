#include "semanticAnalysis/SemanticAnalyser.hpp"

#include "foundations/Database.hpp"
#include "query_compiler/queryparser.hpp"

#include <algorithm>
#include <cassert>
#include <stack>
#include <string>
#include <memory>

void JoinGraph::addVertex(std::string &alias, JoinGraph::Vertex &vertex) {
    vertices.insert({std::move(alias),std::move(vertex)});
}

void JoinGraph::addEdge(JoinGraph::Edge &edge) {
    edges.emplace_back(std::move(edge));
}

/*struct JoinGraph {
    // join graph:
    std::vector<std::unique_ptr<Vertex>> vertices;
    std::unordered_map<std::pair<Vertex *, Vertex *>, std::unique_ptr<Edge>> edges;
    // binding -> vertex
    std::unordered_map<std::string, Vertex *> vertex_mapping;
};

static const Register * create_register_for_constant(const std::string & value, QueryPlan & plan, const Attribute & attr) {
    plan.constant_registers.push_back(std::make_unique<Register>());
    auto & reg = *plan.constant_registers.back();
    switch (attr.getType()) {
        case Attribute::Type::Bool: {
            std::string lowercase;
            std::transform(value.begin(), value.end(), lowercase.begin(), ::tolower);
            if (lowercase == "true") {
                reg.setBool(true);
            } else if (lowercase == "false") {
                reg.setBool(false);
            } else {
                throw std::runtime_error("expected boolean literal");
            }
            break;
        }
        case Attribute::Type::Double:
            reg.setDouble(std::stod(value));
            break;
        case Attribute::Type::Int:
            reg.setInt(std::stoi(value));
            break;
        case Attribute::Type::String:
            reg.setString(value);
            break;
    }
    return &reg;
}*/

/*static std::unique_ptr<Operator> join_connected_components(Vertex & v, std::unique_ptr<Operator> && subtree) {
    assert(!v.visited);

    v.visited = true;
    for (Edge * e : v.edges) {
        Vertex * u = reinterpret_cast<Vertex *>(
                        reinterpret_cast<uintptr_t>(e->vertex1) ^
                        reinterpret_cast<uintptr_t>(e->vertex2) ^
                        reinterpret_cast<uintptr_t>(&v));
        if (u->visited) {
            // we have a cycle -> create additional selections
            for (auto & condition : e->joinConditions) {
                auto chi = std::make_unique<Chi>(std::move(subtree), Chi::Equal, condition.first, condition.second);
                auto chi_result = chi->getResult();
                subtree = std::make_unique<Selection>(std::move(chi), chi_result);
            }
        } else {
            assert(e->joinConditions.size() > 0);
            auto & first_condition = e->joinConditions.front();
            bool left = (&v == e->vertex1);
            if (left) {
                subtree = std::make_unique<HashJoin>(std::move(subtree), std::move(u->input), first_condition.first, first_condition.second);
            } else {
                subtree = std::make_unique<HashJoin>(std::move(subtree), std::move(u->input), first_condition.second, first_condition.first);
            }

            // create the remaining selections
            for (auto it = std::next(e->joinConditions.begin()); it != e->joinConditions.end(); ++it) {
                auto & condition = *it;
                auto chi = std::make_unique<Chi>(std::move(subtree), Chi::Equal, condition.first, condition.second);
                auto chi_result = chi->getResult();
                subtree = std::make_unique<Selection>(std::move(chi), chi_result);
            }

            subtree = join_connected_components(*u, std::move(subtree));
        }
    }

    return std::move(subtree);
}*/

void SemanticAnalyser::construct_join(std::string &vertexName, QueryContext &context, QueryPlan &plan) {
    JoinGraph::Vertex *vertex = plan.graph.getVertex(vertexName);
    std::vector<JoinGraph::Edge*> edges(0);
    plan.graph.getConnectedEdges(vertexName,edges);

    vertex->visited = true;

    if (plan.joinedTree == nullptr) {
        plan.joinedTree = std::move(vertex->production);
    }

    for (auto& edge : edges) {
        std::string &neighboringVertexName = edge->uID;
        if (vertexName.compare(edge->vID) != 0) {
            neighboringVertexName = edge->vID;
        }
        JoinGraph::Vertex *neighboringVertex = plan.graph.getVertex(neighboringVertexName);

        if (neighboringVertex->visited) continue;

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


        construct_join(neighboringVertexName, context, plan);
    }
}

void SemanticAnalyser::construct_joins(QueryContext & context, QueryPlan & plan) {
    std::string firstVertexName = plan.graph.getFirstVertexName();
    construct_join(firstVertexName, context, plan);

    return;
}

void SemanticAnalyser::construct_join_graph(QueryContext & context, QueryPlan & plan) {
    JoinGraph &graph = plan.graph;

    // create vertices
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

        if (!graph.hasEdge(vName,uName)) {
            std::vector<Expressions::exp_op_t> expressions;
            JoinGraph::Edge edge = JoinGraph::Edge(vName,uName,expressions);
            graph.addEdge(edge);
        }

        iu_p_t iuV = plan.ius[vColumn];
        iu_p_t iuU = plan.ius[uColumn];

        std::vector<Expressions::exp_op_t> joinExprVec;
        auto joinExpr = std::make_unique<Expressions::Comparison>(
                Expressions::ComparisonMode::eq, // equijoin
                std::make_unique<Expressions::Identifier>(iuV),
                std::make_unique<Expressions::Identifier>(iuU)
        );
        graph.getEdge(vName,uName)->expressions.push_back(std::move(joinExpr));
    }
}

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

    /*
    auto graph = construct_join_graph(plan);

    auto tree = contruct_join_tree(plan, graph);
    plan.tree = std::move(tree);*/
}
