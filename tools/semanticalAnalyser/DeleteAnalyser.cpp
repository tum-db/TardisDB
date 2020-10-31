//
// Created by Blum Thomas on 2020-06-29.
//

#include "semanticAnalyser/SemanticAnalyser.hpp"

namespace semanticalAnalysis {

    void DeleteAnalyser::verify() {
        Database &db = _context.db;
        DeleteStatement* stmt = _context.parserResult.deleteStmt;
        if (stmt == nullptr) throw semantic_sql_error("unknown statement type");

        if (!db.hasTable(stmt->relation.name)) throw semantic_sql_error("table '" + stmt->relation.name + "' does not exist");
        if (db._branchMapping.find(stmt->relation.version) == db._branchMapping.end()) throw semantic_sql_error("version '" + stmt->relation.version + "' does not exist");
        Table *table = db.getTable(stmt->relation.name);
        std::vector<std::string> columnNames = table->getColumnNames();
        for (auto &column : stmt->selections) {
            if (std::find(columnNames.begin(),columnNames.end(),column.first.name) == columnNames.end())
                throw semantic_sql_error("column '" + column.first.name + "' does not exist");
        }
        // Relation
        // // Relation with name exists?
        // // Branch exists?
        // For each column
        // // Column exists?
    }

    std::unique_ptr<Operator> DeleteAnalyser::constructTree() {
        QueryPlan plan;
        DeleteStatement *stmt = _context.parserResult.deleteStmt;

        std::vector<Relation> relations;
        relations.push_back(stmt->relation);
        construct_scans(_context, plan, relations);
        construct_selects(plan, stmt->selections);

        if (stmt->relation.alias.length() == 0) stmt->relation.alias = stmt->relation.name;
        Table* table = _context.db.getTable(stmt->relation.name);

        iu_p_t tidIU;

        for (auto& production : plan.ius) {
            for (auto &iu : production.second) {
                if (iu.first.compare("tid") == 0) {
                    tidIU = iu.second;
                    break;
                }
            }
        }

        auto &production = plan.dangling_productions[stmt->relation.alias];

        return std::make_unique<Delete>( std::move(production), tidIU, *table);
    }

}
