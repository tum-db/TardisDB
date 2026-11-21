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
        if (needsGeneralAggregation(stmt)) throw general_aggregation_required();
        if (isBranchQuery(stmt)) {
            verifyBranchQuery();
            return;
        }

        // a relation is bound by its alias or, without one, by its name; an unqualified column must
        // belong to exactly one relation
        std::unordered_map<std::string,size_t> unbindedColumns;
        std::unordered_map<std::string,std::vector<std::string>> bindedColumns;
        for (auto &relation : stmt->relations) {
            if (!db.hasTable(relation.name)) throw semantic_sql_error("table '" + relation.name + "' does not exist");
            if (db._branchMapping.find(relation.version) == db._branchMapping.end()) throw semantic_sql_error("version '" + relation.version + "' does not exist");
            const std::string &binding = relation.alias.empty() ? relation.name : relation.alias;
            if (bindedColumns.find(binding) != bindedColumns.end()) {
                if (relation.alias.empty()) throw semantic_sql_error("relation '" + relation.name + "' is already specified - use bindings!");
                throw semantic_sql_error("binding '" + relation.alias + "' is already specified - use bindings!");
            }
            Table *table = db.getTable(relation.name);
            std::vector<std::string> columnNames = table->getColumnNames(db._branchMapping.at(relation.version));
            for (auto &name : columnNames) ++unbindedColumns[name];
            bindedColumns[binding] = std::move(columnNames);
        }

        auto checkColumn = [&](const Column &column) {
            if (column.table.empty()) {
                auto it = unbindedColumns.find(column.name);
                if (it == unbindedColumns.end())
                    throw semantic_sql_error("column '" + column.name + "' is not specified");
                if (it->second > 1)
                    throw semantic_sql_error("column '" + column.name + "' is ambiguous - use bindings!");
            } else {
                auto it = bindedColumns.find(column.table);
                if (it == bindedColumns.end())
                    throw semantic_sql_error("binding '" + column.table + "' is not specified");
                if (std::find(it->second.begin(), it->second.end(), column.name) == it->second.end())
                    throw semantic_sql_error("column '" + column.name + "' is not specified");
            }
        };
        for (auto &column : stmt->selections) checkColumn(column.first);
        for (auto &column : stmt->projections) {
            if (column.expression) continue; // its columns are checked when it is analysed
            checkColumn(column);
        }
        for (auto &column : stmt->joinConditions) {
            checkColumn(column.first);
            checkColumn(column.second);
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
        construct_predicates(_context, stmt->predicates);
        construct_joins(_context);
        for (auto & exp : _context.joinedPredicates) {
            _context.joinedTree = std::make_unique<Select>(std::move(_context.joinedTree), std::move(exp));
        }
        _context.joinedPredicates.clear();

        auto & db = _context.db;

        // the computed columns of the select list
        std::vector<Mapping> mappings;
        std::vector<iu_p_t> computedIUs;
        {
            ExpressionAnalyser analyser(columnResolver(_context));
            for (auto &column : stmt->projections) {
                if (!column.expression) { computedIUs.push_back(nullptr); continue; }
                Expressions::exp_op_t exp = analyser.analyse(*column.expression);
                if (exp->getType().typeID == Sql::SqlType::TypeID::UnknownID) throw semantic_sql_error("the type of '" + column.name + "' is unknown (a NULL without a type)");
                iu_p_t iu = _context.iuFactory.createIU(exp->getType());
                computedIUs.push_back(iu);
                mappings.push_back({iu, std::move(exp)});
            }
        }
        if (!mappings.empty()) _context.joinedTree = std::make_unique<Map>(std::move(_context.joinedTree), std::move(mappings));

        //get projected IUs
        std::vector<iu_p_t> projectedIUs;
        for (size_t p = 0; p < stmt->projections.size(); ++p) {
            auto &column = stmt->projections[p];
            if (computedIUs[p] != nullptr) {
                projectedIUs.push_back(computedIUs[p]);
            } else if (column.table.length() == 0) {
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
                for (auto &name : db.getTable(relation.name)->getColumnNames(db._branchMapping.at(relation.version))) {
                    auto it = ius.find(name);
                    if (it != ius.end()) projectedIUs.push_back(it->second);
                }
            }
        }

        _context.joinedTree = std::make_unique<Result>( std::move(_context.joinedTree), projectedIUs );
    }

    bool containsAggregate(const sqlExpressions::Node & n) {
        using Kind = sqlExpressions::Node::Kind;
        if (n.kind == Kind::Function && (n.name == "count" || n.name == "sum" || n.name == "avg" || n.name == "min" || n.name == "max")) {
            return true;
        }
        for (auto & child : n.children) if (containsAggregate(child)) return true;
        return false;
    }

    bool SelectAnalyser::needsGeneralAggregation(SelectStatement *stmt) {
        // queries over several branches (VERSION ALL, ...) and those of the branch column stay with BranchAggregation
        for (auto &relation : stmt->relations) {
            if (!relation.versionScope.empty()) return false;
        }
        bool aggregates = false, expressions = false, branchColumn = false;
        for (auto &column : stmt->projections) {
            if (!column.aggregate.empty()) aggregates = true;
            if (column.expression) {
                expressions = true;
                if (containsAggregate(*column.expression)) aggregates = true;
            }
            if (column.table.empty() && column.name == "branch" && column.aggregate.empty() && !column.expression) branchColumn = true;
        }
        bool groupByColumns = false;
        for (auto &column : stmt->groupBy) {
            if (column.name == "branch" && column.table.empty()) branchColumn = true;
            else groupByColumns = true;
        }
        if (!aggregates && stmt->groupBy.empty()) return false;
        if (branchColumn) return false;
        if (groupByColumns || expressions || stmt->relations.size() != 1 || !stmt->joinConditions.empty()) return true;
        // aggregates of a column: BranchAggregation computes numbers (and counts)
        Relation &relation = stmt->relations.front();
        Table *table = _context.db.getTable(relation.name);
        auto branch = _context.db._branchMapping.find(relation.version);
        if (table == nullptr || branch == _context.db._branchMapping.end()) return false; // the errors of the branch query
        for (auto &column : stmt->projections) {
            if (column.aggregate.empty() || column.aggregate == "count") continue;
            ci_p_t ci = table->findColumn(column.name, branch->second);
            if (ci == nullptr) return false;
            const auto id = ci->type.typeID;
            if (id != Sql::SqlType::TypeID::IntegerID && id != Sql::SqlType::TypeID::LongIntegerID &&
                id != Sql::SqlType::TypeID::NumericID && id != Sql::SqlType::TypeID::DoubleID) return true;
        }
        return false;
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
            // the private branches of transactions are neither selected nor make their parent an inner branch
            std::set<branch_id_t> parents;
            for (auto &[id, branch] : db._branches) if (!branch->transaction) parents.insert(branch->parent_id);
            for (auto &[id, branch] : db._branches) {
                if (branch->transaction) continue;
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
        for (auto &column : stmt->projections) {
            if (column.expression) throw semantic_sql_error("queries over several branches take columns and aggregates of columns, not expressions");
        }
        if (stmt->relations.size() != 1 || !stmt->joinConditions.empty())
            throw semantic_sql_error("queries over several branches read exactly one table");
        Relation &relation = stmt->relations.front();
        if (!db.hasTable(relation.name)) throw semantic_sql_error("table '" + relation.name + "' does not exist");
        Table *table = db.getTable(relation.name);
        const auto branches = resolveBranches(db, relation);
        const size_t branchCount = branches.size();

        auto checkColumn = [&](const Column &column) {
            if (!column.table.empty() && column.table != relation.name && column.table != relation.alias)
                throw semantic_sql_error("binding '" + column.table + "' is not specified");
            // branches may have different columns of the same name (ALTER TABLE ... VERSION)
            ci_p_t ci = table->findColumn(column.name, branches);
            if (ci == nullptr)
                throw semantic_sql_error("column '" + column.name + "' is not specified" +
                                         (branchCount > 1 ? " in every branch read" : ""));
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
                type != Sql::SqlType::TypeID::LongIntegerID && type != Sql::SqlType::TypeID::NumericID &&
                type != Sql::SqlType::TypeID::DoubleID)
                throw semantic_sql_error(column.aggregate + "(" + column.name + ") needs a numeric column");
        }
        if (stmt->projections.empty()) throw semantic_sql_error("queries over several branches need aggregates or GROUP BY branch, not *");
        if (projectsBranch && !groupByBranch) throw semantic_sql_error("column 'branch' must appear in GROUP BY");
        if (!aggregates && !groupByBranch) throw semantic_sql_error("queries over several branches need aggregates or GROUP BY branch");
        if (branchCount > 1 && !groupByBranch) throw semantic_sql_error("aggregates over several branches need GROUP BY branch");
        for (auto &predicate : stmt->predicates) {
            for (auto &column : predicate.columns) checkColumn(column); // those of any condition are checked when it is analysed
        }
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
            // count(x) of a column without nulls counts the tuples, of a nullable column its values
            const bool countsTuples = function == BranchAggregation::Function::Count &&
                    (column.name == "*" || !table->findColumn(column.name, branches)->type.nullable);
            iu_p_t input = countsTuples ? nullptr : aggregation->getColumnIU(column.name);
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
        // the columns of conditions: those that every branch has
        auto resolve = [&](const std::string &binding, const std::string &name) -> iu_p_t {
            if (!binding.empty() && binding != relation.name && binding != relation.alias)
                throw semantic_sql_error("binding '" + binding + "' is not specified");
            return table->findColumn(name, branches) != nullptr ? aggregation->getColumnIU(name) : nullptr;
        };
        for (auto &predicate : stmt->predicates) {
            if (predicate.op == "expr") {
                ExpressionAnalyser analyser(resolve);
                aggregation->addPredicate(analyser.analyseCondition(*predicate.expression));
                continue;
            }
            std::vector<iu_p_t> ius;
            for (auto &column : predicate.columns) ius.push_back(aggregation->getColumnIU(column.name));
            aggregation->addPredicate(predicateExpression(predicate, ius));
        }

        _context.joinedTree = std::make_unique<Result>(std::move(aggregation), projectedIUs);
    }

    std::vector<std::string> SelectAnalyser::diffColumnNames(Table &table, Relation &relation) {
        // the columns both branches have, in the order of the first
        Database &db = _context.db;
        const std::vector<branch_id_t> branches = {db._branchMapping.at(relation.versionSet[0]), db._branchMapping.at(relation.versionSet[1])};
        std::vector<std::string> names;
        for (auto &name : table.getColumnNames(branches[0])) {
            if (table.findColumn(name, branches) != nullptr) names.push_back(name);
        }
        return names;
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
        for (auto &column : stmt->projections) {
            if (column.expression) throw semantic_sql_error("DIFF takes columns, not expressions");
        }
        if (stmt->relations.size() != 1 || !stmt->joinConditions.empty())
            throw semantic_sql_error("DIFF cannot be combined with other relations yet");
        if (!stmt->groupBy.empty()) throw semantic_sql_error("DIFF does not support GROUP BY yet");
        Relation &relation = stmt->relations.front();
        Table *table = db.getTable(relation.name);
        if (table == nullptr) throw semantic_sql_error("table '" + relation.name + "' does not exist");
        for (auto &name : relation.versionSet) {
            if (db._branchMapping.find(name) == db._branchMapping.end()) throw semantic_sql_error("version '" + name + "' does not exist");
        }

        const auto columnNames = diffColumnNames(*table, relation);
        auto checkColumn = [&](const Column &column) {
            if (!column.table.empty() && column.table != relation.name && column.table != relation.alias)
                throw semantic_sql_error("binding '" + column.table + "' is not specified");
            if (std::find(columnNames.begin(), columnNames.end(), column.name) == columnNames.end())
                throw semantic_sql_error("column '" + column.name + "' is not specified in both branches");
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
        for (auto &predicate : stmt->predicates) {
            for (auto &column : predicate.columns) checkColumn(column);
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
            for (auto &name : diffColumnNames(*table, relation)) projectedIUs.push_back(diff->getColumnIU(name));
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
        const auto columnNames = diffColumnNames(*table, relation);
        auto resolve = [&](const std::string &binding, const std::string &name) -> iu_p_t {
            if (!binding.empty() && binding != relation.name && binding != relation.alias)
                throw semantic_sql_error("binding '" + binding + "' is not specified");
            if (std::find(columnNames.begin(), columnNames.end(), name) == columnNames.end()) return nullptr;
            return diff->getColumnIU(name);
        };
        for (auto &predicate : stmt->predicates) {
            if (predicate.op == "expr") {
                ExpressionAnalyser analyser(resolve);
                diff->addPredicate(analyser.analyseCondition(*predicate.expression));
                continue;
            }
            std::vector<iu_p_t> ius;
            for (auto &column : predicate.columns) ius.push_back(diff->getColumnIU(column.name));
            diff->addPredicate(predicateExpression(predicate, ius));
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

        // the tables that no condition joins with the others: cross products (a join without a condition,
        // whose conditions over several tables apply above the joins)
        for (auto & name : context.graph.getVertexNames()) {
            if (context.graph.getVertex(name)->visited) continue;
            auto joined = std::move(context.joinedTree);
            construct_join(context, name);
            context.joinedTree = std::make_unique<Join>(std::move(joined), std::move(context.joinedTree),
                                                        std::vector<Expressions::exp_op_t>(), Join::Method::Hash);
        }

        if (context.joinedTree == nullptr)
            throw semantic_sql_error("no or more than one root found: Table joining has failed");
    }

}