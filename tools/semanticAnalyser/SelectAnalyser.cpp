//
// Created by Blum Thomas on 2020-06-29.
//

#include "semanticAnalyser/SemanticAnalyser.hpp"

namespace semanticalAnalysis {

    /// Plans the subqueries of FROM (into context.derivedTables), with the names of their columns
    static void analyseSubqueries(AnalyzingContext & context, SelectStatement & stmt) {
        for (auto & relation : stmt.relations) {
            if (!relation.subquery || context.derivedTables.count(relation.name) > 0) continue;
            auto nested = std::make_unique<AnalyzingContext>(context.db);
            nested->parserResult.opType = SQLParserResult::OpType::Select;
            nested->parserResult.selectStmt = new SelectStatement(*relation.subquery);
            SelectStatement & sub = *nested->parserResult.selectStmt;
            SelectAnalyser analyser(*nested);
            analyser.verify();
            analyser.constructTree();
            auto * root = dynamic_cast<Result *>(nested->joinedTree.get());
            if (root == nullptr) throw semantic_sql_error("a subquery in FROM is a query");
            AnalyzingContext::DerivedTable derived;
            derived.ius = root->selection;
            // the names: aliases, names of columns and aggregates, else of the columns the IUs read
            for (size_t c = 0; c < derived.ius.size(); ++c) {
                std::string name = "?column?";
                if (sub.projections.size() == derived.ius.size()) {
                    auto & projection = sub.projections[c];
                    name = !projection.alias.empty() ? projection.alias : projection.aggregate.empty() ? projection.name : projection.aggregate;
                } else if (derived.ius[c]->iuType == InformationUnit::Type::ColumnRef) {
                    name = derived.ius[c]->columnInformation->columnName;
                }
                derived.names.push_back(name);
            }
            if (relation.subqueryColumns.size() > derived.names.size()) {
                throw semantic_sql_error("a subquery in FROM has " + std::to_string(derived.names.size()) + " columns available but " +
                                         std::to_string(relation.subqueryColumns.size()) + " columns specified");
            }
            for (size_t c = 0; c < relation.subqueryColumns.size(); ++c) derived.names[c] = relation.subqueryColumns[c];
            std::set<std::string> unique;
            for (auto & name : derived.names) {
                if (!unique.insert(name).second) throw semantic_sql_error("column name \"" + name + "\" appears more than once in a subquery in FROM");
            }
            derived.tree = root->takeChild();
            context.branchIds.insert(nested->branchIds.begin(), nested->branchIds.end());
            context.derivedTables.emplace(relation.name, std::move(derived));
            context.nested.push_back(std::move(nested));
        }
    }

    /// \returns The names of the columns of a relation in its branch (of a subquery: of its select list)
    static std::vector<std::string> relationColumns(AnalyzingContext & context, const Relation & relation) {
        auto derived = context.derivedTables.find(relation.name);
        if (derived != context.derivedTables.end()) return derived->second.names;
        return context.db.getTable(relation.name)->getColumnNames(context.db._branchMapping.at(relation.version));
    }

    /// Puts the columns of the relations, in their order, in place of a * among other items of the select list
    static void expandStars(AnalyzingContext & context, SelectStatement & stmt) {
        Database & db = context.db;
        // SELECT * of subqueries: their columns under their names
        if (stmt.projections.empty() && std::any_of(stmt.relations.begin(), stmt.relations.end(), [](const Relation & r) { return r.subquery != nullptr; })) {
            stmt.projections.push_back(Column());
            stmt.projections.back().name = "*";
        }
        std::vector<Column> projections;
        for (auto & column : stmt.projections) {
            if (column.name != "*" || !column.aggregate.empty() || column.expression) {
                projections.push_back(std::move(column));
                continue;
            }
            for (auto & relation : stmt.relations) {
                if (relation.name == sqlExpressions::oneRowTable) throw semantic_sql_error("SELECT * with no tables specified is not valid");
                if (!relation.subquery) {
                    if (!db.hasTable(relation.name)) throw semantic_sql_error("table '" + relation.name + "' does not exist");
                    if (db._branchMapping.find(relation.version) == db._branchMapping.end()) throw semantic_sql_error("version '" + relation.version + "' does not exist");
                }
                for (auto & name : relationColumns(context, relation)) {
                    Column expanded;
                    expanded.name = name;
                    expanded.table = relation.alias.empty() ? relation.name : relation.alias;
                    projections.push_back(std::move(expanded));
                }
            }
        }
        stmt.projections = std::move(projections);
    }

    void SelectAnalyser::verify() {
        Database &db = _context.db;
        SelectStatement* stmt = _context.parserResult.selectStmt;
        if (stmt == nullptr) throw semantic_sql_error("unknown statement type");
        analyseSubqueries(_context, *stmt);
        expandStars(_context, *stmt);
        if (isDiffQuery(stmt)) {
            verifyDiffQuery();
            return;
        }
        _aggregation = needsAggregation(stmt);
        if (!_aggregation && isBranchQuery(stmt)) {
            verifyBranchQuery();
            return;
        }

        // a relation is bound by its alias or, without one, by its name; an unqualified column must
        // belong to exactly one relation
        std::unordered_map<std::string,size_t> unbindedColumns;
        std::unordered_map<std::string,std::vector<std::string>> bindedColumns;
        for (auto &relation : stmt->relations) {
            if (!relation.subquery) {
                if (!db.hasTable(relation.name)) throw semantic_sql_error("table '" + relation.name + "' does not exist");
                if (db._branchMapping.find(relation.version) == db._branchMapping.end()) throw semantic_sql_error("version '" + relation.version + "' does not exist");
            }
            const std::string &binding = relation.alias.empty() ? relation.name : relation.alias;
            if (bindedColumns.find(binding) != bindedColumns.end()) {
                if (relation.alias.empty()) throw semantic_sql_error("relation '" + relation.name + "' is already specified - use bindings!");
                throw semantic_sql_error("binding '" + relation.alias + "' is already specified - use bindings!");
            }
            std::vector<std::string> columnNames = relationColumns(_context, relation);
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
            if (column.aggregate == "count" && column.name == "*") continue;
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
        if (!_aggregation && isBranchQuery(stmt)) {
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
        if (_aggregation) {
            constructAggregation(stmt);
            return;
        }

        auto & db = _context.db;

        // the computed columns of the select list, over the results of its window functions
        std::vector<Mapping> mappings;
        std::vector<iu_p_t> computedIUs;
        {
            std::vector<sqlExpressions::Node> expressions;
            for (auto & column : stmt->projections) if (column.expression) expressions.push_back(*column.expression);
            ColumnResolver resolve = columnResolver(_context);
            if (std::any_of(expressions.begin(), expressions.end(), [](const sqlExpressions::Node & n) { return containsWindow(n); })) {
                resolve = constructWindows(expressions, resolve);
            }
            ExpressionAnalyser analyser(resolve);
            size_t next = 0;
            for (size_t c = 0; c < stmt->projections.size(); ++c) {
                auto & column = stmt->projections[c];
                if (!column.expression) { computedIUs.push_back(nullptr); continue; }
                const sqlExpressions::Node & expression = expressions[next++];
                Expressions::exp_op_t exp = analyser.analyse(expression);
                if (exp->getType().typeID == Sql::SqlType::TypeID::UnknownID) {
                    // a NULL of the select list is text, as in PostgreSQL, or of the column an INSERT writes it into
                    const Sql::SqlType type = projectionType(c);
                    exp = analyser.analyse(expression, &type);
                }
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
                for (auto &name : relationColumns(_context, relation)) {
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
        if (n.window) {
            const auto & w = *n.window;
            for (auto * nodes : {&w.partition, &w.order, &w.startOffset, &w.endOffset}) {
                for (auto & child : *nodes) if (containsAggregate(child)) return true;
            }
        }
        return false;
    }

    void SelectAnalyser::constructAggregation(SelectStatement *stmt) {
        using sqlExpressions::Node;
        using Function = Aggregation::Function;
        if (stmt->projections.empty()) throw semantic_sql_error("SELECT * cannot be used with aggregates or GROUP BY");
        ColumnResolver resolve = columnResolver(_context);
        ExpressionAnalyser inputs(resolve);
        auto column = [](const std::string & name, const std::string & table = std::string()) {
            Node n;
            n.kind = Node::Kind::Column;
            n.name = name;
            n.table = table;
            return n;
        };

        // the keys
        std::vector<Aggregation::Key> keys;
        for (auto & key : stmt->groupBy) {
            iu_p_t iu = resolve(key.table, key.name);
            if (iu == nullptr) throw semantic_sql_error("column '" + key.name + "' is not specified");
            keys.push_back({iu, _context.iuFactory.createIU(iu->sqlType)});
        }
        auto group = [&](const std::string & table, const std::string & name) {
            for (size_t j = 0; j < stmt->groupBy.size(); ++j) {
                auto & key = stmt->groupBy[j];
                if (key.name == name && (key.table == table || key.table.empty() || table.empty())) return j;
            }
            throw semantic_sql_error("column \"" + (table.empty() ? name : table + "." + name) +
                                     "\" must appear in the GROUP BY clause or be used in an aggregate function");
        };

        // the aggregates, of columns or of expressions computed below them
        std::vector<Aggregation::Aggregate> aggregates;
        std::vector<Mapping> arguments;
        auto aggregate = [&](const std::string & name, bool star, const Node * argument) {
            const Function function = name == "count" ? (star ? Function::CountAll : Function::Count) : name == "sum" ? Function::Sum :
                                      name == "avg" ? Function::Avg : name == "min" ? Function::Min : Function::Max;
            iu_p_t input = nullptr;
            if (!star) {
                using ID = Sql::SqlType::TypeID;
                if (containsAggregate(*argument)) throw semantic_sql_error("aggregate function calls cannot be nested");
                if (argument->kind == Node::Kind::Column) {
                    input = resolve(argument->table, argument->name);
                    if (input == nullptr) throw semantic_sql_error("column '" + argument->name + "' is not specified");
                }
                auto isString = [](Sql::SqlType type) { return type.typeID == ID::CharID || type.typeID == ID::VarcharID; };
                const bool extreme = function == Function::Min || function == Function::Max;
                if (input == nullptr || (extreme && isString(input->sqlType))) {
                    // an expression; the least or greatest char or varchar as text, NULL as NULL of integer (no values)
                    Expressions::exp_op_t e = inputs.analyse(*argument);
                    if (extreme && isString(e->getType())) {
                        Node text;
                        text.kind = Node::Kind::Cast;
                        text.name = "text";
                        text.children = {*argument};
                        e = inputs.analyse(text);
                    }
                    if (e->getType().typeID == ID::UnknownID) {
                        const Sql::SqlType integer = Sql::getIntegerTy(true);
                        e = inputs.analyse(*argument, &integer);
                    }
                    input = _context.iuFactory.createIU(e->getType());
                    arguments.push_back({input, std::move(e)});
                }
            }
            Sql::SqlType result;
            if (!Aggregation::aggregateType(function, star ? Sql::getIntegerTy() : Sql::toNotNullableTy(input->sqlType), result)) {
                const std::string type = Sql::getName(Sql::toNotNullableTy(input->sqlType));
                if (function == Function::Sum || function == Function::Avg) throw semantic_sql_error(name + " takes numbers or arrays of numbers, not " + type);
                throw semantic_sql_error("function " + name + "(" + type + ") does not exist");
            }
            aggregates.push_back({function, input, _context.iuFactory.createIU(result)});
            return aggregates.size() - 1;
        };
        // an expression over the groups: aggregates and keys as columns \x01a<k> and \x01g<j>
        std::function<Node(const Node &)> rewrite = [&](const Node & n) -> Node {
            if (n.kind == Node::Kind::Function && (n.name == "count" || n.name == "sum" || n.name == "avg" || n.name == "min" || n.name == "max")) {
                const bool star = n.name == "count" && n.value == "*" && n.children.empty();
                if (!star && n.children.size() != 1) throw semantic_sql_error("the aggregate " + n.name + " takes one argument");
                return column("\x01" "a" + std::to_string(aggregate(n.name, star, star ? nullptr : &n.children[0])));
            }
            if (n.kind == Node::Kind::Column) return column("\x01" "g" + std::to_string(group(n.table, n.name)));
            Node copy = n;
            for (auto & child : copy.children) child = rewrite(child);
            if (n.window) {
                // the window of a window function over the groups
                auto window = std::make_shared<sqlExpressions::Window>(*n.window);
                for (auto * nodes : {&window->partition, &window->order, &window->startOffset, &window->endOffset}) {
                    for (auto & child : *nodes) child = rewrite(child);
                }
                copy.window = window;
            }
            return copy;
        };
        std::vector<Node> outputs;
        for (auto & projection : stmt->projections) {
            if (!projection.aggregate.empty()) {
                const bool star = projection.name == "*";
                const Node argument = column(projection.name, projection.table);
                outputs.push_back(column("\x01" "a" + std::to_string(aggregate(projection.aggregate, star, star ? nullptr : &argument))));
            } else if (projection.expression) {
                outputs.push_back(rewrite(*projection.expression));
            } else if (projection.name == "*") {
                throw semantic_sql_error("SELECT * cannot be used with aggregates or GROUP BY");
            } else {
                outputs.push_back(column("\x01" "g" + std::to_string(group(projection.table, projection.name))));
            }
        }
        std::unique_ptr<Node> having;
        if (stmt->having) having = std::make_unique<Node>(rewrite(*stmt->having));

        // the arguments, the groups, HAVING and the select list over them
        if (!arguments.empty()) _context.joinedTree = std::make_unique<Map>(std::move(_context.joinedTree), std::move(arguments));
        _context.joinedTree = std::make_unique<Aggregation>(std::move(_context.joinedTree), keys, aggregates);
        ColumnResolver groupColumns = [&](const std::string &, const std::string & name) -> iu_p_t {
            if (name.size() < 3 || name[0] != '\x01') return nullptr;
            const size_t index = std::stoul(name.substr(2));
            return name[1] == 'a' ? aggregates.at(index).output : keys.at(index).output;
        };
        ExpressionAnalyser groups(groupColumns);
        if (having) _context.joinedTree = std::make_unique<Select>(std::move(_context.joinedTree), groups.analyseCondition(*having));
        // the window functions over the groups
        if (std::any_of(outputs.begin(), outputs.end(), [](const Node & n) { return containsWindow(n); })) {
            groups = ExpressionAnalyser(constructWindows(outputs, groupColumns));
        }
        std::vector<Mapping> mappings;
        std::vector<iu_p_t> projected;
        for (size_t c = 0; c < outputs.size(); ++c) {
            Expressions::exp_op_t e = groups.analyse(outputs[c]);
            if (auto * identifier = dynamic_cast<Expressions::Identifier *>(e.get())) {
                projected.push_back(identifier->_iu);
                continue;
            }
            if (e->getType().typeID == Sql::SqlType::TypeID::UnknownID) {
                const Sql::SqlType type = projectionType(c);
                e = groups.analyse(outputs[c], &type);
            }
            iu_p_t iu = _context.iuFactory.createIU(e->getType());
            projected.push_back(iu);
            mappings.push_back({iu, std::move(e)});
        }
        if (!mappings.empty()) _context.joinedTree = std::make_unique<Map>(std::move(_context.joinedTree), std::move(mappings));
        _context.joinedTree = std::make_unique<Result>(std::move(_context.joinedTree), projected);
    }

    bool SelectAnalyser::needsAggregation(SelectStatement *stmt) {
        // queries over several branches (VERSION ALL, ...) and those of the branch column stay with BranchAggregation
        for (auto &relation : stmt->relations) {
            if (!relation.versionScope.empty()) {
                if (stmt->having) throw semantic_sql_error("HAVING is not supported in queries over several branches");
                return false;
            }
        }
        bool aggregates = false, branchColumn = false;
        for (auto &column : stmt->projections) {
            if (!column.aggregate.empty()) aggregates = true;
            if (column.expression && containsAggregate(*column.expression)) aggregates = true;
            if (column.table.empty() && column.name == "branch" && column.aggregate.empty() && !column.expression) branchColumn = true;
        }
        for (auto &column : stmt->groupBy) {
            if (column.name == "branch" && column.table.empty()) branchColumn = true;
        }
        if (stmt->having) {
            // the condition over the groups is a WHERE over the table of the groups
            if (branchColumn) throw semantic_sql_error("HAVING is not supported with the column branch");
            return true;
        }
        if (!aggregates && stmt->groupBy.empty()) return false;
        if (branchColumn) return false;
        // the aggregates of one branch, also of columns of a table: an Aggregation of the plan
        return true;
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
        if (stmt->having) throw semantic_sql_error("DIFF does not support HAVING");
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
            // a copy: addVertex moves from it, and SELECT * reads the alias of the relation afterwards
            std::string tableAlias = rel.alias.empty() ? rel.name : rel.alias;
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

            iu_p_t iuV = context.ius[vNode.table][vNode.name];
            iu_p_t iuU = context.ius[uNode.table][uNode.name];
            addJoinCondition(graph, vNode.table, uNode.table, std::make_unique<Expressions::Comparison>(
                    Expressions::ComparisonMode::eq, // equijoin
                    std::make_unique<Expressions::Identifier>(iuV),
                    std::make_unique<Expressions::Identifier>(iuU)));
        }

        // the conditions left = right of WHERE whose sides read one table each, as k.id = up.id - 1, join too
        // (sides of different types stay conditions above the join: their values would hash differently)
        std::unordered_map<iu_p_t, std::string> productionOf;
        for (auto & [production, columns] : context.ius) {
            for (auto & [name, iu] : columns) productionOf[iu] = production;
        }
        auto tableOf = [&](const Expressions::Expression & side) { // the one table that a side reads, else ""
            std::string table;
            for (iu_p_t iu : collectRequired(side)) {
                auto it = productionOf.find(iu);
                if (it == productionOf.end() || (!table.empty() && table != it->second)) return std::string();
                table = it->second;
            }
            return table;
        };
        std::vector<Expressions::exp_op_t> remaining;
        for (auto & exp : context.joinedPredicates) {
            auto * cmp = dynamic_cast<Expressions::Comparison *>(exp.get());
            if (cmp != nullptr && cmp->_mode == Expressions::ComparisonMode::eq &&
                Sql::equals(cmp->getLeftChild().getType(), cmp->getRightChild().getType(), Sql::SqlTypeEqualsMode::WithoutNullable)) {
                std::string left = tableOf(cmp->getLeftChild()), right = tableOf(cmp->getRightChild());
                if (!left.empty() && !right.empty() && left != right) {
                    addJoinCondition(graph, left, right, std::move(exp));
                    continue;
                }
            }
            remaining.push_back(std::move(exp));
        }
        context.joinedPredicates = std::move(remaining);
    }

    /// Adds the condition left = right (\p leftTable = \p rightTable) to the edge of both tables, with the side
    /// of the edge's first table (vID) on the left, as construct_join builds the join
    void SelectAnalyser::addJoinCondition(JoinGraph & graph, std::string leftTable, std::string rightTable, Expressions::exp_op_t condition) {
        if (!graph.hasEdge(leftTable, rightTable)) {
            std::vector<Expressions::exp_op_t> none;
            JoinGraph::Edge edge(leftTable, rightTable, none);
            graph.addEdge(edge);
        }
        JoinGraph::Edge * edge = graph.getEdge(leftTable, rightTable);
        if (edge->vID != leftTable) dynamic_cast<Expressions::Comparison &>(*condition).swapSides();
        edge->expressions.push_back(std::move(condition));
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

            // a neighbor joined already (a cycle of conditions): the conditions of the edge apply above the joins
            if (neighboringVertex->visited) {
                for (auto & exp : edge->expressions) context.joinedPredicates.push_back(std::move(exp));
                edge->expressions.clear();
                continue;
            }

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