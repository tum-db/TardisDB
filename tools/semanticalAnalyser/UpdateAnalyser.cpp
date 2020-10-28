//
// Created by Blum Thomas on 2020-06-29.
//

#include "semanticAnalyser/SemanticAnalyser.hpp"

namespace semanticalAnalysis {

    void UpdateAnalyser::verify() {
        Database &db = _context.analyzingContext.db;
        UpdateStatement* stmt = _parserResult.updateStmt;
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
    std::unique_ptr<Operator> UpdateAnalyser::constructTree() {
        QueryPlan plan;
        UpdateStatement *stmt = _parserResult.updateStmt;

        std::vector<Relation> relations;
        relations.push_back(stmt->relation);
        construct_scans(_context, plan, relations);
        construct_selects(plan, stmt->selections);

        Table* table = _context.analyzingContext.db.getTable(stmt->relation.name);
        if (stmt->relation.alias.length() == 0) stmt->relation.alias = stmt->relation.name;

        branch_id_t branchId;
        if (stmt->relation.version.compare("master") != 0) {
            branchId = _context.analyzingContext.db._branchMapping[stmt->relation.version];
        } else {
            branchId = master_branch_id;
        }

        std::vector<std::pair<iu_p_t,std::string>> updateIUs;

        //Get all ius of the tuple to update
        for (auto& production : plan.ius) {
            for (auto &iu : production.second) {
                updateIUs.emplace_back( iu.second, "" );
            }
        }

        //Map values to be updated to the corresponding ius
        for (auto &[column,value] : stmt->updates) {
            for (auto &iuPair : updateIUs) {
                if (iuPair.first->columnInformation->columnName.compare(column.name) == 0) {
                    iuPair.second = value;
                }
            }
        }

        auto &production = plan.dangling_productions[stmt->relation.alias];

        return std::make_unique<Update>( std::move(production), updateIUs, *table, _context.analyzingContext.db._branchMapping[stmt->relation.version]);
    }

}