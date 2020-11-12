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

    void DeleteAnalyser::constructTree() {
        DeleteStatement *stmt = _context.parserResult.deleteStmt;

        construct_scans(_context, stmt->relation);
        construct_selects(_context, stmt->selections);

        if (stmt->relation.alias.length() == 0) stmt->relation.alias = stmt->relation.name;
        branch_id_t branchId = (stmt->relation.version.compare("master") != 0) ?
                               _context.db._branchMapping[stmt->relation.version] :
                               master_branch_id;

        Table* table = _context.db.getTable(stmt->relation.name);
        iu_p_t tidIU = _context.getUniqueColumnIU("tid");
        auto &production = _context.dangling_productions[stmt->relation.alias];

        _context.joinedTree = std::make_unique<Delete>( std::move(production), tidIU, *table, branchId);
    }

}
