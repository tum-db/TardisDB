//
// Created by Blum Thomas on 2020-06-29.
//

#include "semanticAnalyser/SemanticAnalyser.hpp"

namespace semanticalAnalysis {

    void SelectAnalyser::verify() {
        Database &db = _context.analyzingContext.db;
        SelectStatement* stmt = _parserResult.selectStmt;
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
    std::unique_ptr<Operator> SelectAnalyser::constructTree() {
        QueryPlan plan;
        SelectStatement *stmt = _parserResult.selectStmt;

        construct_scans(_context, plan, stmt->relations);
        construct_selects(plan, stmt->selections);
        construct_joins(_context,plan, _parserResult);

        auto & db = _context.analyzingContext.db;

        //get projected IUs
        std::vector<iu_p_t> projectedIUs;
        for (auto &column : stmt->projections) {
            if (column.table.length() == 0) {
                for (auto& production : plan.ius) {
                    for (auto& iu : production.second) {
                        if (iu.first.compare(column.name) == 0) {
                            projectedIUs.push_back( iu.second );
                        }
                    }
                }
            } else {
                projectedIUs.push_back(plan.ius[column.table][column.name]);
            }
        }

        if (plan.joinedTree != nullptr) {
            // Construct Result and store it in the query plan struct
            return std::make_unique<Result>( std::move(plan.joinedTree), projectedIUs );
        } else {
            throw std::runtime_error("no or more than one root found: Table joining has failed");
        }
    }

}