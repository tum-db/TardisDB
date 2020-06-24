#include "include/tardisdb/semanticAnalyser/SemanticAnalyser.hpp"

#include "foundations/Database.hpp"
#include "native/sql/SqlValues.hpp"
#include "foundations/version_management.hpp"

#include <algorithm>
#include <cassert>
#include <stack>
#include <string>
#include <memory>
#include <native/sql/SqlTuple.hpp>

void SemanticAnalyser::construct_scans(QueryContext& context, QueryPlan & plan) {
    auto& db = context.db;

    if (plan.parser_result.versions.size() != plan.parser_result.relations.size()) {
        return;
    }

    size_t i = 0;
    for (auto& relation : plan.parser_result.relations) {
        std::string tableName = relation.first;
        std::string tableAlias = relation.second;
        std::string &branchName = plan.parser_result.versions[i];

        Table* table = db.getTable(tableName);

        // Recognize version
        if (branchName.compare("master") != 0) {
            context.executionContext.branchIds.insert({tableAlias,db._branchMapping[branchName]});
        } else {
            context.executionContext.branchIds.insert({tableAlias,0});
        }


        //Construct the logical TableScan operator
        std::unique_ptr<TableScan> scan = std::make_unique<TableScan>(context, *table, new std::string(tableAlias));

        //Store the ius produced by this TableScan
        const iu_set_t& produced = scan->getProduced();
        for (iu_p_t iu : produced) {
            plan.ius[tableAlias][iu->columnInformation->columnName] = iu;
            plan.iuNameToTable[iu->columnInformation->columnName] = table;
        }

        //Add a new production with TableScan as root node
        plan.dangling_productions.insert({tableAlias, std::move(scan)});
        i++;
    }
}

void SemanticAnalyser::construct_selects(QueryContext& context, QueryPlan& plan) {
    auto & db = context.db;
    for (auto & selection : plan.parser_result.selections) {
        std::string& productionName = selection.first.first;
        std::string& productionIUName = selection.first.second;
        std::string& valueString = selection.second;

        iu_p_t iu = plan.ius[productionName][productionIUName];

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

void SemanticAnalyser::construct_joins(QueryContext & context, QueryPlan & plan) {
    //Start with the first vertex in the vector of vertices of the join graph
    std::string firstVertexName = plan.graph.getFirstVertexName();

    //Construct join the first vertex
    construct_join(firstVertexName, context, plan);
}

//TODO: Make projections available to every node in the tree
//TODO: Check physical tree projections do not work after joining
void SemanticAnalyser::construct_projection(QueryContext& context, QueryPlan & plan) {
    auto & db = context.db;

    //get projected IUs
    std::vector<iu_p_t> projectedIUs;
    for (const std::string& projectedIUName : plan.parser_result.projections) {
        if (plan.iuNameToTable.find(projectedIUName) == plan.iuNameToTable.end()) {
            throw std::runtime_error("column " + projectedIUName + " not in scope");
        }
        for (auto& production : plan.ius) {
            for (auto& iu : production.second) {
                if (iu.first.compare(projectedIUName) == 0) {
                    projectedIUs.push_back( iu.second );
                }
            }
        }

    }

    if (plan.joinedTree != nullptr) {
        // Construct Result and store it in the query plan struct
        plan.tree = std::make_unique<Result>( std::move(plan.joinedTree), projectedIUs );
    } else {
        throw std::runtime_error("no or more than one root found: Table joining has failed");
    }
}

void SemanticAnalyser::construct_update(QueryContext& context, QueryPlan & plan) {
    if (plan.dangling_productions.size() != 1 || plan.parser_result.relations.size() != 1) {
        throw std::runtime_error("no or more than one root found: Table joining has failed");
    }

    auto &relationName = plan.parser_result.relations[0].second;
    Table* table = context.db.getTable(plan.parser_result.relations[0].first);

    std::vector<std::pair<iu_p_t,std::string>> updateIUs;

    //Get all ius of the tuple to update
    for (auto& production : plan.ius) {
        for (auto &iu : production.second) {
            updateIUs.emplace_back( iu.second, "" );
        }
    }

    //Map values to be updated to the corresponding ius
    for (auto columnValuePairs : plan.parser_result.columnToValue) {
        const std::string &valueString = columnValuePairs.second;

        for (auto &iuPair : updateIUs) {
            if (iuPair.first->columnInformation->columnName.compare(columnValuePairs.first) == 0) {
                iuPair.second = valueString;
            }
        }
    }

    auto &production = plan.dangling_productions[relationName];
    plan.tree = std::make_unique<Update>( std::move(production), updateIUs, *table, new std::string(relationName));
}

void SemanticAnalyser::construct_delete(QueryContext& context, QueryPlan & plan) {
    if (plan.dangling_productions.size() != 1 || plan.parser_result.relations.size() != 1) {
        throw std::runtime_error("no or more than one root found: Table joining has failed");
    }

    auto &relationName = plan.parser_result.relations[0].second;
    Table* table = context.db.getTable(plan.parser_result.relations[0].first);

    iu_p_t tidIU;

    for (auto& production : plan.ius) {
        for (auto &iu : production.second) {
            if (iu.first.compare("tid") == 0) {
                tidIU = iu.second;
                break;
            }
        }
    }

    auto &production = plan.dangling_productions[relationName];
    plan.tree = std::make_unique<Delete>( std::move(production), tidIU, *table);
}

void SemanticAnalyser::constructSelect(QueryContext& context, QueryPlan& plan) {
    construct_scans(context, plan);
    construct_selects(context, plan);
    construct_join_graph(context,plan);
    construct_joins(context,plan);
    construct_projection(context, plan);
}

void SemanticAnalyser::constructInsert(QueryContext &context, QueryPlan &plan) {
    auto& db = context.db;
    Table* table = db.getTable(plan.parser_result.relation);

    if (plan.parser_result.versions.size() == 1) {
        std::string &branchName = plan.parser_result.versions[0];
        if (branchName.compare("master") != 0) {
            context.executionContext.branchId = db._branchMapping[branchName];
        } else {
            context.executionContext.branchId = 0;
        }
    } else {
        return;
    }

    std::vector<std::unique_ptr<Native::Sql::Value>> sqlvalues;

    for (int i=0; i<plan.parser_result.columnNames.size(); i++) {
        Native::Sql::SqlType type = table->getCI(plan.parser_result.columnNames[i])->type;
        std::string &value = plan.parser_result.values[i];
        std::unique_ptr<Native::Sql::Value> sqlvalue = Native::Sql::Value::castString(value,type);

        sqlvalues.emplace_back(std::move(sqlvalue));
    }

    Native::Sql::SqlTuple *tuple =  new Native::Sql::SqlTuple(std::move(sqlvalues));

    plan.tree = std::make_unique<Insert>(context,*table,tuple);
}

void SemanticAnalyser::constructUpdate(QueryContext& context, QueryPlan& plan) {
    construct_scans(context, plan);
    construct_selects(context, plan);
    construct_update(context, plan);
}

void SemanticAnalyser::constructDelete(QueryContext& context, QueryPlan& plan) {
    construct_scans(context, plan);
    construct_selects(context, plan);
    construct_delete(context, plan);
}

void SemanticAnalyser::constructCreate(QueryContext& context, QueryPlan& plan) {
    auto & createdTable = context.db.createTable(plan.parser_result.relation);

    for (int i = 0; i < plan.parser_result.columnNames.size(); i++) {
        std::string &columnName = plan.parser_result.columnNames[i];
        std::string &columnType = plan.parser_result.columnTypes[i];
        bool nullable = plan.parser_result.nullable[i];
        uint32_t length = plan.parser_result.length[i];
        uint32_t precision = plan.parser_result.precision[i];

        Sql::SqlType sqlType;
        if (columnType.compare("bool") == 0) {
            sqlType = Sql::getBoolTy(nullable);
        } else if (columnType.compare("date") == 0) {
            sqlType = Sql::getDateTy(nullable);
        } else if (columnType.compare("integer") == 0) {
            sqlType = Sql::getIntegerTy(nullable);
        } else if (columnType.compare("longinteger") == 0) {
            sqlType = Sql::getLongIntegerTy(nullable);
        } else if (columnType.compare("numeric") == 0) {
            sqlType = Sql::getNumericTy(length,precision,nullable);
        } else if (columnType.compare("char") == 0) {
            sqlType = Sql::getCharTy(length, nullable);
        } else if (columnType.compare("varchar") == 0) {
            sqlType = Sql::getVarcharTy(length, nullable);
        } else if (columnType.compare("timestamp") == 0) {
            sqlType = Sql::getTimestampTy(nullable);
        } else if (columnType.compare("text") == 0) {
            sqlType = Sql::getTextTy(nullable);
        }

        createdTable.addColumn(columnName, sqlType);
    }
}

void SemanticAnalyser::constructCheckout(QueryContext& context, QueryPlan& plan) {
    // Get branchName and parentBranchId from statement
    std::string &branchName = plan.parser_result.branchId;
    std::string &parentBranchName = plan.parser_result.parentBranchId;

    // Search for mapped branchId of parentBranchName
    branch_id_t _parentBranchId = 0;
    if (parentBranchName.compare("master") != 0) {
        _parentBranchId = context.db._branchMapping[parentBranchName];
    }

    // Add new branch
    branch_id_t branchid = context.db.createBranch(branchName, _parentBranchId);

    std::cout << "Created Branch " << branchid << "\n";
}

// identifier -> (binding, Attribute)
using scope_t = std::unordered_map<std::string, std::pair<std::string,ci_p_t>>;

bool in_scope(const scope_t & scope, const SQLParserResult::BindingAttribute & binding_attr) {
    std::string identifier = binding_attr.first + "." + binding_attr.second;
    return scope.count(identifier) > 0;
}

scope_t construct_scope(Database& db, const SQLParserResult & result) {
    scope_t scope;
    for (auto & rel_pair : result.relations) {
        if (!db.hasTable(rel_pair.first)) {
            throw incorrect_sql_error("unknown relation '" + rel_pair.first + "'");
        }
        auto * table = db.getTable(rel_pair.first);
        size_t attr_cnt = table->getColumnCount();
        for (size_t i = 0; i < attr_cnt; ++i) {
            auto * attr = table->getCI(table->getColumnNames()[i]);
            scope[rel_pair.second + "." + attr->columnName] = std::make_pair(rel_pair.second, attr);
            if (scope.count(attr->columnName) > 0) {
                scope[attr->columnName].second = nullptr;
            } else {
                scope[attr->columnName] = std::make_pair(rel_pair.second, attr);
            }
        }
    }
    return scope;
}

SQLParserResult::BindingAttribute fully_qualify(const SQLParserResult::BindingAttribute & current, const scope_t & scope) {
    if (!current.first.empty()) {
        return current;
    }
    auto it = scope.find(current.second);
    if (it == scope.end()) {
        throw incorrect_sql_error("unknown column '" + current.second + "'");
    }
    return SQLParserResult::BindingAttribute(it->second.first, current.second);
}

void fully_qualify_names(const scope_t & scope, SQLParserResult & result) {
    for (size_t i = 0; i < result.selections.size(); ++i) {
        auto & [binding_attr, _] = result.selections[i];
        result.selections[i].first = fully_qualify(binding_attr, scope);
    }

    for (size_t i = 0; i < result.joinConditions.size(); ++i) {
        auto & [lhs, rhs] = result.joinConditions[i];
        result.joinConditions[i].first = fully_qualify(lhs, scope);
        result.joinConditions[i].second = fully_qualify(rhs, scope);
    }

    // TODO same for result.projections
}

void validate_sql_statement(const scope_t & scope, Database& db, const SQLParserResult & result) {
    for (auto & attr_name : result.projections) {
        auto it = scope.find(attr_name);
        if (it == scope.end()) {
            throw incorrect_sql_error("unknown column '" + attr_name + "'");
        } else if (it->second.second == nullptr) {
            throw incorrect_sql_error("'" + attr_name + "' is ambiguous");
        }
    }

    for (auto & selection_pair : result.selections) {
        auto & binding_attr = selection_pair.first;
        if (!in_scope(scope, binding_attr)) {
            throw incorrect_sql_error("unknown column '" + binding_attr.first + "." + binding_attr.second + "'");
        }
    }

    for (auto & join_pair : result.joinConditions) {
        auto & lhs = join_pair.first;
        if (!in_scope(scope, lhs)) {
            throw incorrect_sql_error("unknown column '" + lhs.first + "." + lhs.second + "'");
        }
        auto & rhs = join_pair.second;
        if (!in_scope(scope, rhs)) {
            throw incorrect_sql_error("unknown column '" + rhs.first + "." + rhs.second + "'");
        }
    }
}

void SemanticAnalyser::analyse_sql_statement(Database& db, SQLParserResult &result) {
    auto scope = construct_scope(db, result);
    fully_qualify_names(scope, result);
    validate_sql_statement(scope, db, result);
}

std::unique_ptr<Operator> SemanticAnalyser::parse_and_construct_tree(QueryContext& context, std::string sql) {
    QueryPlan plan;
    plan.parser_result = parse_sql_statement(sql);
    analyse_sql_statement(context.db, plan.parser_result);

    switch (plan.parser_result.opType) {
        case SQLParserResult::OpType::Select:
            constructSelect(context, plan);
            break;
        case SQLParserResult::OpType::Insert:
            constructInsert(context, plan);
            break;
        case SQLParserResult::OpType::Update:
            constructUpdate(context, plan);
            break;
        case SQLParserResult::OpType::Delete:
            constructDelete(context, plan);
            break;
        case SQLParserResult::OpType::CreateTable:
            constructCreate(context, plan);
            break;
        case SQLParserResult::OpType::CreateBranch:
            constructCheckout(context, plan);
            break;
    }

    return std::move(plan.tree);
}

