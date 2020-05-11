#include "SQLToOperator.hpp"

#include "foundations/Database.hpp"
#include "query_compiler/queryparser.hpp"

#include <algorithm>
#include <cassert>
#include <stack>
#include <string>
#include <memory>

/*struct Edge;

struct Vertex {
    std::string binding_name;
    std::unique_ptr<Operator> input;
    std::vector<Edge *> edges;
    size_t cardinality = 0;
    bool visited = false;

    Vertex(const std::string & binding_name, std::unique_ptr<Operator> && input)
        : binding_name(binding_name)
        , input(std::move(input))
    { }
};

struct Edge {
    Vertex * vertex1, * vertex2;
    std::vector<std::pair<const Register *, const Register *>> joinConditions; // the provided hashjoin actually only supports one comparision
    double selectivity;

    Edge(Vertex * v, Vertex * u)
        : vertex1(v)
        , vertex2(u)
    { }
};

struct JoinGraph {
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

void construct_selects(QueryContext& context, QueryPlan& plan) {
    auto & db = context.db;
    for (auto & selection : plan.parser_result.selections) {
        auto& scan = plan.dangling_subtrees[selection.first.first];

        auto it1 = plan.attributeToTable.find(selection.first.second);
        if (it1 == plan.attributeToTable.end()) {
            throw std::runtime_error("column " + selection.first.second + " not in scope");
        }
        Table * table = it1->second;
        iu_p_t attr = plan.scope[selection.first.second];
        std::string value = selection.second;
        ci_p_t ci = table->getCI(selection.first.second);

        if (ci->type.nullable) {
            throw NotImplementedException();
        }
        auto constExp = std::make_unique<Expressions::Constant>(value, ci->type);
        auto identifier = std::make_unique<Expressions::Identifier>(attr);
        auto exp = std::make_unique<Expressions::Comparison>(
                Expressions::ComparisonMode::eq,
                std::move(identifier),
                std::move(constExp)
        );

        // construct the select operator
        auto select = std::make_unique<Select>(std::move(plan.dangling_subtrees[selection.first.first]), std::move(exp));
        plan.dangling_subtrees[selection.first.first] = std::move(select);
    }
}

void construct_scans(QueryContext& context, QueryPlan & plan) {
    auto & db = context.db;
    for (auto & relation : plan.parser_result.relations) {
        auto* table = db.getTable(relation.first);

        auto scan = std::make_unique<TableScan>(context, *table);

        auto& produced = scan->getProduced();
        for (iu_p_t iu : produced) {
            auto ci = getColumnInformation(iu);
            plan.scope[ci->columnName] = iu;
            plan.attributeToTable[ci->columnName] = table;
        }

        plan.dangling_subtrees.insert({relation.second, std::move(scan)});
    }
}

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
}

static std::unique_ptr<Operator> contruct_join_tree(QueryPlan & plan, JoinGraph & graph) {
    std::vector<std::unique_ptr<Operator>> subtrees;

    for (auto & v : graph.vertices) {
        if (v->visited) {
            continue;
        }
        std::unique_ptr<Operator> subtree = std::move(v->input);
        subtrees.push_back(join_connected_components(*v, std::move(subtree)));
    }

    auto tree = std::move(subtrees.back());
    subtrees.pop_back();
    // all constraints are exhausted -> add some cross products
    for (auto & subtree : subtrees) {
        tree = std::make_unique<CrossProduct>(std::move(tree), std::move(subtree));
    }
    return std::move(tree);
}

static JoinGraph construct_join_graph(QueryPlan & plan) {
    JoinGraph graph;

    // create vertices
    for (auto & rel : plan.parser_result.relations) {
        graph.vertices.push_back(std::make_unique<Vertex>(rel.second, std::move(plan.dangling_subtrees[rel.second])));
        Vertex * v = graph.vertices.back().get();
        graph.vertex_mapping.insert({rel.second, v});
    }

    // create edges
    for (auto & join_condition : plan.parser_result.joinConditions) {
        Vertex * v = graph.vertex_mapping[join_condition.first.first];
        Vertex * u = graph.vertex_mapping[join_condition.second.first];

        Edge * e;
        auto k = std::minmax(v, u);
        auto it = graph.edges.find(k);
        if (it != graph.edges.end()) {
            e = it->second.get();
        } else {
            // create a new edge
            graph.edges.emplace(k, std::make_unique<Edge>(u, v));
            it = graph.edges.find(k);
            assert(it != graph.edges.end());
            e = it->second.get();

            v->edges.push_back(e);
            u->edges.push_back(e);
        }

        const auto reg1 = plan.scans[v->binding_name]->getOutput(join_condition.first.second);
        const auto reg2 = plan.scans[u->binding_name]->getOutput(join_condition.second.second);
        bool left = (v == e->vertex1);
        if (left) {
            e->joinConditions.emplace_back(reg1, reg2);
        } else {
            e->joinConditions.emplace_back(reg2, reg1);
        }
    }

    return graph;
}*/

void construct_projection(QueryContext& context, QueryPlan & plan) {
    auto & db = context.db;

    std::vector<iu_p_t> projections;
    for (const std::string & attr : plan.parser_result.projections) {
        if (plan.attributeToTable.find(attr) == plan.attributeToTable.end()) {
            throw std::runtime_error("column " + attr + " not in scope");
        }
        projections.push_back( plan.scope[attr] );
    }

    if (plan.dangling_subtrees.size() == 1) {
        plan.tree = std::make_unique<Result>( std::move(plan.dangling_subtrees.begin()->second), projections );
    } else {
        throw std::runtime_error("more than one root found: Table joining has failed");
    }
}

void construct_tree(QueryContext& context, QueryPlan& plan) {
    construct_scans(context, plan);
    construct_selects(context, plan);
    construct_projection(context, plan);
}


std::unique_ptr<Result> parse_and_construct_tree(QueryContext& context, std::string sql) {
    QueryPlan plan;
    plan.parser_result = parse_and_analyse_sql_statement(context.db, sql);

    construct_tree(context, plan);

    return std::move(plan.tree);
    //return plan;

    /*construct_scans(db, plan);
    auto graph = construct_join_graph(plan);

    auto tree = contruct_join_tree(plan, graph);
    plan.tree = std::move(tree);

    construct_projection(db, plan);

    PlanContainer pc;
    pc.root = std::move(plan.tree);
    pc.impl = std::move(impl);
    return std::move(pc);*/
}
