//
// Created by Blum Thomas on 2020-06-29.
//

#include "semanticAnalyser/SemanticAnalyser.hpp"

namespace semanticalAnalysis {

    void UpdateAnalyser::verify() {
        Database &db = _context.db;
        UpdateStatement* stmt = _context.parserResult.updateStmt;
        if (stmt == nullptr) throw semantic_sql_error("unknown statement type");

        if (!db.hasTable(stmt->relation.name)) throw semantic_sql_error("table '" + stmt->relation.name + "' does not exist");
        if (db._branchMapping.find(stmt->relation.version) == db._branchMapping.end()) throw semantic_sql_error("version '" + stmt->relation.version + "' does not exist");
        Table *table = db.getTable(stmt->relation.name);
        std::vector<std::string> columnNames = table->getColumnNames();
        for (auto &column : stmt->updates) {
            if (std::find(columnNames.begin(),columnNames.end(),column.first.name) == columnNames.end())
                throw semantic_sql_error("column '" + column.first.name + "' does not exist");
        }
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

    //TODO: Verifier check on only one table to update
    void UpdateAnalyser::constructTree() {
        UpdateStatement *stmt = _context.parserResult.updateStmt;

        construct_scans(_context, stmt->relation);
        construct_selects(_context, stmt->selections);

        Table* table = _context.db.getTable(stmt->relation.name);
        if (stmt->relation.alias.length() == 0) stmt->relation.alias = stmt->relation.name;

        branch_id_t branchId = (stmt->relation.version.compare("master") != 0) ?
            _context.db._branchMapping[stmt->relation.version] :
            master_branch_id;

        //Check for every IU if an update is specified and set the update value to an empty string if not
        std::vector<std::pair<iu_p_t,std::string>> updateIUs;
        for (auto &iu : _context.ius[stmt->relation.alias]) {
            bool shouldBeUpdated = false;
            for (auto &[column,value] : stmt->updates) {
                shouldBeUpdated = iu.first.compare(column.name) == 0;
                if (shouldBeUpdated) {
                    updateIUs.emplace_back( iu.second,value);
                    break;
                }
            }
            if (!shouldBeUpdated) updateIUs.emplace_back( iu.second,"");
        }

        auto &production = _context.dangling_productions[stmt->relation.alias];
        _context.joinedTree = std::make_unique<Update>( std::move(production), updateIUs, *table, branchId);
    }

}