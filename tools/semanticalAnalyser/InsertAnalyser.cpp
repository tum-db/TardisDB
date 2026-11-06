//
// Created by Blum Thomas on 2020-06-29.
//

#include "semanticAnalyser/SemanticAnalyser.hpp"
#include "foundations/version_management.hpp"

namespace semanticalAnalysis {

    void InsertAnalyser::verify() {
        Database &db = _context.db;
        InsertStatement* stmt = _context.parserResult.insertStmt;
        if (stmt == nullptr) throw semantic_sql_error("unknown statement type");

        if (!db.hasTable(stmt->relation.name)) throw semantic_sql_error("table '" + stmt->relation.name + "' does not exist");
        if (db._branchMapping.find(stmt->relation.version) == db._branchMapping.end()) throw semantic_sql_error("version '" + stmt->relation.version + "' does not exist");
        Table *table = db.getTable(stmt->relation.name);
        std::vector<std::string> columnNames = table->getColumnNames(db._branchMapping.at(stmt->relation.version));
        auto checkRow = [&](const std::vector<std::string> & row) {
            if (stmt->columns.size() == 0 && row.size() != columnNames.size() )
                throw semantic_sql_error("values for columns of table '" + stmt->relation.name + "' must be specified");
            if (stmt->columns.size() != 0 && row.size() != stmt->columns.size())
                throw semantic_sql_error("the numbers of columns and values differ");
        };
        checkRow(stmt->values);
        for (auto & row : stmt->rows) checkRow(row);
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
                db._branchMapping.at(branchName):
                master_branch_id;

        if (stmt->columns.empty()) {
            for (auto &column : table->getColumnNames(branchId)) {
                stmt->columns.push_back(Column());
                stmt->columns.back().name = column;
            }
        }
        std::vector<ci_p_t> cis;
        for (auto &column : stmt->columns) cis.push_back(table->findColumn(column.name, branchId));

        // one tuple per row of VALUES; the columns the branch does not have, and those not given,
        // get their defaults
        std::vector<Native::Sql::SqlTuple *> tuples;
        auto addRow = [&](const std::vector<std::string> & row) {
            std::vector<std::pair<ci_p_t, std::unique_ptr<Native::Sql::Value>>> sqlvalues;
            for (size_t i = 0; i < cis.size(); i++) {
                sqlvalues.emplace_back(cis[i], Native::Sql::Value::castString(row[i], cis[i]->type));
            }
            tuples.push_back(complete_tuple(*table, std::move(sqlvalues)).release());
        };
        addRow(stmt->values);
        for (auto & row : stmt->rows) addRow(row);

        _context.joinedTree = std::make_unique<Insert>(_context.iuFactory,*table,move(tuples),branchId);
    }

}