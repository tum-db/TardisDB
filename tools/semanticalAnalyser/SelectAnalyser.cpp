//
// Created by Blum Thomas on 2020-06-29.
//

#include "semanticAnalyser/SemanticAnalyser.hpp"

namespace semanticalAnalysis {

    void SelectAnalyser::verify() {
        Database &db = _context.db;
        SelectStatement* stmt = _context.parserResult.selectStmt;
        if (stmt == nullptr) throw semantic_sql_error("unknown statement type");
        if (isDiffQuery(stmt)) {
            verifyDiffQuery();
            return;
        }
        if (isBranchQuery(stmt)) {
            verifyBranchQuery();
            return;
        }

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
        if (isDiffQuery(stmt)) {
            constructDiffQuery();
            return;
        }
        if (isBranchQuery(stmt)) {
            constructBranchQuery();
            return;
        }

        construct_scans(_context, stmt->relations);
        construct_selects(_context, stmt->selections);
        construct_joins(_context);

        auto & db = _context.db;

        //get projected IUs
        std::vector<iu_p_t> projectedIUs;
        for (auto &column : stmt->projections) {
            if (column.table.length() == 0) {
                projectedIUs.push_back(_context.getUniqueColumnIU(column.name));
            } else {
                projectedIUs.push_back(_context.ius[column.table][column.name]);
            }
        }
        if (stmt->projections.empty()) {
            // SELECT *: the columns in the order of their tables
            for (auto &relation : stmt->relations) {
                std::string &productionName = relation.alias.empty() ? relation.name : relation.alias;
                auto &ius = _context.ius[productionName];
                for (auto &name : db.getTable(relation.name)->getColumnNames()) {
                    auto it = ius.find(name);
                    if (it != ius.end()) projectedIUs.push_back(it->second);
                }
            }
        }

        _context.joinedTree = std::make_unique<Result>( std::move(_context.joinedTree), projectedIUs );
    }

    bool SelectAnalyser::isBranchQuery(SelectStatement *stmt) {
        for (auto &column : stmt->projections) {
            if (!column.aggregate.empty() || (column.table.empty() && column.name == "branch")) return true;
        }
        for (auto &relation : stmt->relations) {
            if (!relation.versionScope.empty()) return true;
        }
        return !stmt->groupBy.empty();
    }

    std::vector<branch_id_t> SelectAnalyser::resolveBranches(Database &db, Relation &relation) {
        auto lookup = [&](const std::string &name) {
            auto it = db._branchMapping.find(name);
            if (it == db._branchMapping.end()) throw semantic_sql_error("version '" + name + "' does not exist");
            return it->second;
        };
        std::vector<branch_id_t> branches;
        if (relation.versionScope.empty()) {
            branches.push_back(lookup(relation.version));
        } else if (relation.versionScope == "set") {
            for (auto &name : relation.versionSet) {
                branch_id_t branch = lookup(name);
                if (std::find(branches.begin(), branches.end(), branch) == branches.end()) branches.push_back(branch);
            }
        } else {
            const branch_id_t root = lookup(relation.version);
            std::set<branch_id_t> parents;
            for (auto &[id, branch] : db._branches) parents.insert(branch->parent_id);
            for (auto &[id, branch] : db._branches) {
                bool selected = (relation.versionScope == "all");
                if (!selected) {
                    // a descendant of the root, without the root itself
                    for (branch_id_t b = branch->parent_id; b != invalid_branch_id; b = db._branches.at(b)->parent_id) {
                        if (b == root) { selected = true; break; }
                    }
                    if (relation.versionScope == "leaves" && parents.count(id) > 0) selected = false;
                }
                if (selected) branches.push_back(id);
            }
            std::sort(branches.begin(), branches.end());
        }
        if (branches.empty()) throw semantic_sql_error("no branch matches the version of '" + relation.name + "'");
        return branches;
    }

    void SelectAnalyser::verifyBranchQuery() {
        Database &db = _context.db;
        SelectStatement *stmt = _context.parserResult.selectStmt;
        if (stmt->relations.size() != 1 || !stmt->joinConditions.empty())
            throw semantic_sql_error("queries over several branches read exactly one table");
        Relation &relation = stmt->relations.front();
        if (!db.hasTable(relation.name)) throw semantic_sql_error("table '" + relation.name + "' does not exist");
        Table *table = db.getTable(relation.name);
        const size_t branchCount = resolveBranches(db, relation).size();

        auto checkColumn = [&](const Column &column) {
            if (!column.table.empty() && column.table != relation.name && column.table != relation.alias)
                throw semantic_sql_error("binding '" + column.table + "' is not specified");
            const auto columnNames = table->getColumnNames();
            if (std::find(columnNames.begin(), columnNames.end(), column.name) == columnNames.end())
                throw semantic_sql_error("column '" + column.name + "' is not specified");
            ci_p_t ci = table->getCI(column.name);
            if (ci->type.nullable) throw semantic_sql_error("column '" + column.name + "' is nullable, which queries over several branches do not support yet");
            return ci;
        };

        bool groupByBranch = false;
        for (auto &column : stmt->groupBy) {
            if (!column.table.empty() || column.name != "branch")
                throw semantic_sql_error("only GROUP BY branch is supported");
            groupByBranch = true;
        }
        bool projectsBranch = false;
        bool aggregates = false;
        for (auto &column : stmt->projections) {
            if (column.aggregate.empty()) {
                if (!column.table.empty() || column.name != "branch")
                    throw semantic_sql_error("column '" + column.name + "' must be aggregated, since only GROUP BY branch is supported");
                projectsBranch = true;
                continue;
            }
            aggregates = true;
            if (column.name == "*") continue;
            ci_p_t ci = checkColumn(column);
            auto type = ci->type.typeID;
            if (column.aggregate != "count" && type != Sql::SqlType::TypeID::IntegerID &&
                type != Sql::SqlType::TypeID::LongIntegerID && type != Sql::SqlType::TypeID::NumericID)
                throw semantic_sql_error(column.aggregate + "(" + column.name + ") needs a numeric column");
        }
        if (stmt->projections.empty()) throw semantic_sql_error("queries over several branches need aggregates or GROUP BY branch, not *");
        if (projectsBranch && !groupByBranch) throw semantic_sql_error("column 'branch' must appear in GROUP BY");
        if (!aggregates && !groupByBranch) throw semantic_sql_error("queries over several branches need aggregates or GROUP BY branch");
        if (branchCount > 1 && !groupByBranch) throw semantic_sql_error("aggregates over several branches need GROUP BY branch");
        for (auto &selection : stmt->selections) checkColumn(selection.first);
    }

    void SelectAnalyser::constructBranchQuery() {
        Database &db = _context.db;
        SelectStatement *stmt = _context.parserResult.selectStmt;
        Relation &relation = stmt->relations.front();
        Table *table = db.getTable(relation.name);

        std::vector<branch_id_t> branches = resolveBranches(db, relation);
        _context.branchIds.insert(branches.begin(), branches.end());
        auto aggregation = std::make_unique<BranchAggregation>(_context.iuFactory, *table, branches);

        std::vector<iu_p_t> projectedIUs;
        for (auto &column : stmt->projections) {
            if (column.aggregate.empty()) {
                projectedIUs.push_back(aggregation->addBranchOutput());
                continue;
            }
            BranchAggregation::Function function = BranchAggregation::Function::Count;
            if (column.aggregate == "sum") function = BranchAggregation::Function::Sum;
            else if (column.aggregate == "min") function = BranchAggregation::Function::Min;
            else if (column.aggregate == "max") function = BranchAggregation::Function::Max;
            else if (column.aggregate == "avg") function = BranchAggregation::Function::Avg;
            // count(x) of a column without nulls counts the tuples
            iu_p_t input = (function == BranchAggregation::Function::Count) ? nullptr : aggregation->getColumnIU(column.name);
            projectedIUs.push_back(aggregation->addAggregate(function, input));
        }

        for (auto &[column, valueString] : stmt->selections) {
            iu_p_t iu = aggregation->getColumnIU(column.name);
            aggregation->addPredicate(std::make_unique<Expressions::Comparison>(
                    Expressions::ComparisonMode::eq,
                    std::make_unique<Expressions::Identifier>(iu),
                    std::make_unique<Expressions::Constant>(valueString, iu->columnInformation->type)
            ));
        }

        _context.joinedTree = std::make_unique<Result>(std::move(aggregation), projectedIUs);
    }

    bool SelectAnalyser::isDiffQuery(SelectStatement *stmt) {
        for (auto &relation : stmt->relations) {
            if (relation.versionScope == "diff") return true;
        }
        return false;
    }

    void SelectAnalyser::verifyDiffQuery() {
        Database &db = _context.db;
        SelectStatement *stmt = _context.parserResult.selectStmt;
        if (stmt->relations.size() != 1 || !stmt->joinConditions.empty())
            throw semantic_sql_error("DIFF cannot be combined with other relations yet");
        if (!stmt->groupBy.empty()) throw semantic_sql_error("DIFF does not support GROUP BY yet");
        Relation &relation = stmt->relations.front();
        Table *table = db.getTable(relation.name);
        if (table == nullptr) throw semantic_sql_error("table '" + relation.name + "' does not exist");
        for (auto &name : relation.versionSet) {
            if (db._branchMapping.find(name) == db._branchMapping.end()) throw semantic_sql_error("version '" + name + "' does not exist");
        }

        const auto columnNames = table->getColumnNames();
        auto checkColumn = [&](const Column &column) {
            if (!column.table.empty() && column.table != relation.name && column.table != relation.alias)
                throw semantic_sql_error("binding '" + column.table + "' is not specified");
            if (std::find(columnNames.begin(), columnNames.end(), column.name) == columnNames.end())
                throw semantic_sql_error("column '" + column.name + "' is not specified");
            if (table->getCI(column.name)->type.nullable)
                throw semantic_sql_error("column '" + column.name + "' is nullable, which DIFF does not support yet");
        };
        for (auto &column : stmt->projections) {
            if (!column.aggregate.empty()) throw semantic_sql_error("DIFF does not support aggregates yet");
            if (column.table.empty() && column.name == "branch") continue;
            checkColumn(column);
        }
        if (stmt->projections.empty()) {
            for (auto &name : columnNames) {
                Column column;
                column.name = name;
                checkColumn(column);
            }
        }
        for (auto &selection : stmt->selections) checkColumn(selection.first);
    }

    void SelectAnalyser::constructDiffQuery() {
        Database &db = _context.db;
        SelectStatement *stmt = _context.parserResult.selectStmt;
        Relation &relation = stmt->relations.front();
        Table *table = db.getTable(relation.name);
        const branch_id_t first = db._branchMapping.at(relation.versionSet[0]);
        const branch_id_t second = db._branchMapping.at(relation.versionSet[1]);
        _context.branchIds.insert({first, second});

        auto diff = std::make_unique<BranchDiff>(_context.iuFactory, *table, first, second);
        std::vector<iu_p_t> projectedIUs;
        if (stmt->projections.empty()) {
            // SELECT *: the branch, then the columns of the table
            projectedIUs.push_back(diff->getBranchIU());
            for (auto &name : table->getColumnNames()) projectedIUs.push_back(diff->getColumnIU(name));
        }
        for (auto &column : stmt->projections) {
            if (column.table.empty() && column.name == "branch") projectedIUs.push_back(diff->getBranchIU());
            else projectedIUs.push_back(diff->getColumnIU(column.name));
        }
        for (auto &[column, valueString] : stmt->selections) {
            iu_p_t iu = diff->getColumnIU(column.name);
            diff->addPredicate(std::make_unique<Expressions::Comparison>(
                    Expressions::ComparisonMode::eq,
                    std::make_unique<Expressions::Identifier>(iu),
                    std::make_unique<Expressions::Constant>(valueString, iu->columnInformation->type)
            ));
        }

        _context.joinedTree = std::make_unique<Result>(std::move(diff), projectedIUs);
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
            throw semantic_sql_error("no or more than one root found: Table joining has failed");
    }

}