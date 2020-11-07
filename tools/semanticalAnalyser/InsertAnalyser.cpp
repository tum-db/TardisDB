//
// Created by Blum Thomas on 2020-06-29.
//

#include "semanticAnalyser/SemanticAnalyser.hpp"

namespace semanticalAnalysis {

    void InsertAnalyser::verify() {
        Database &db = _context.db;
        InsertStatement* stmt = _context.parserResult.insertStmt;
        if (stmt == nullptr) throw semantic_sql_error("unknown statement type");

        if (!db.hasTable(stmt->relation.name)) throw semantic_sql_error("table '" + stmt->relation.name + "' does not exist");
        if (db._branchMapping.find(stmt->relation.version) == db._branchMapping.end()) throw semantic_sql_error("version '" + stmt->relation.version + "' does not exist");
        Table *table = db.getTable(stmt->relation.name);
        std::vector<std::string> columnNames = table->getColumnNames();
        if (stmt->columns.size() == 0 && stmt->values.size() != table->getColumnNames().size() )
            throw semantic_sql_error("values for columns of table '" + stmt->relation.name + "' must be specified");
        for (auto &column : stmt->columns) {
            if (std::find(columnNames.begin(),columnNames.end(),column.name) == columnNames.end())
                throw semantic_sql_error("column '" + column.name + "' does not exist");
        }
        // Relation
        // // Relation with name exists?
        // // Branch exists?
        // For each column
        // // Column exists?
    }

    void InsertAnalyser::constructTree() {
        InsertStatement *stmt = _context.parserResult.insertStmt;

        auto& db = _context.db;
        Table* table = db.getTable(stmt->relation.name);

        std::string &branchName = stmt->relation.version;
        branch_id_t branchId = (branchName.compare("master") != 0) ?
                db._branchMapping[branchName]:
                master_branch_id;

        std::vector<std::unique_ptr<Native::Sql::Value>> sqlvalues;
        if (stmt->columns.empty()) {
            for (auto &column : table->getColumnNames()) {
                stmt->columns.push_back(Column());
                stmt->columns.back().name = column;
            }
        }
        for (int i=0; i<stmt->columns.size(); i++) {
            Native::Sql::SqlType type = table->getCI(stmt->columns[i].name)->type;
            std::string &value = stmt->values[i];
            std::unique_ptr<Native::Sql::Value> sqlvalue = Native::Sql::Value::castString(value,type);

            sqlvalues.emplace_back(std::move(sqlvalue));
        }

        Native::Sql::SqlTuple *tuple =  new Native::Sql::SqlTuple(std::move(sqlvalues));

        _context.joinedTree = std::make_unique<Insert>(_context.iuFactory,*table,tuple,branchId);
    }

}