//
// Created by Blum Thomas on 2020-06-29.
//

#include "semanticAnalyser/SemanticalVerifier.hpp"
#include "foundations/exceptions.hpp"
#include "algebra/logical/operators.hpp"

namespace semanticalAnalysis {

    void addToScope(QueryContext & context, iu_p_t iu, const std::string & symbol)
    {
        context.analyzingContext.scope.emplace(symbol, iu);
    }

    void addToScope(QueryContext & context, Algebra::Logical::TableScan & scan)
    {
        auto & scope = context.analyzingContext.scope;
        for (iu_p_t iu : scan.getProduced()) {
            ci_p_t ci = getColumnInformation(iu);
            scope.emplace(ci->columnName, iu);
        }
    }

    void addToScope(QueryContext & context, Algebra::Logical::TableScan & scan, const std::string & prefix)
    {
        auto & scope = context.analyzingContext.scope;
        for (iu_p_t iu : scan.getProduced()) {
            ci_p_t ci = getColumnInformation(iu);
            scope.emplace(prefix + "." + ci->columnName, iu);
        }
    }

    iu_p_t lookup(QueryContext & context, const std::string & symbol)
    {
        auto & scope = context.analyzingContext.scope;
        auto it = scope.find(symbol);
        if (it == scope.end()) {
            throw ParserException("unknown identifier: " + symbol);
        }
        return it->second;
    }

    void SemanticalVerifier::verifyCreateTable(SQLParserResult &result) {
        Database &db = _context.analyzingContext.db;
        CreateTableStatement* stmt = result.createTableStmt;

        if (stmt == nullptr) throw semantic_sql_error("unknown statement type");
        if (db.hasTable(stmt->tableName)) throw semantic_sql_error("table '" + stmt->tableName + "' already exists");
        std::vector<std::string> definedColumnNames;
        std::vector<std::string> typeNames = {"bool","date","integer","longinteger","numeric","char","varchar","timestamp","text"};
        for (auto &column : stmt->columns) {
            if (std::find(definedColumnNames.begin(),definedColumnNames.end(),column.name) != definedColumnNames.end())
                throw semantic_sql_error("column '" + column.name + "' already exists");
            definedColumnNames.push_back(column.name);
            if (std::find(typeNames.begin(),typeNames.end(),column.type) == typeNames.end())
                throw semantic_sql_error("type '" + column.type + "' does not exist");
        }

        // Table already exists?
        // For each columnspec
        // // name already exists?
        // // type exists?
    }

    void SemanticalVerifier::verifyCreateBranch(SQLParserResult &result) {
        Database &db = _context.analyzingContext.db;
        CreateBranchStatement* stmt = result.createBranchStmt;
        if (stmt == nullptr) throw semantic_sql_error("unknown statement type");

        if (db._branchMapping.find(stmt->branchName) != db._branchMapping.end())
            throw semantic_sql_error("branch '" + stmt->branchName + "' already exists");
        if (db._branchMapping.find(stmt->parentBranchName) == db._branchMapping.end())
            throw semantic_sql_error("parent branch '" + stmt->parentBranchName + "' does not exist");
        // Branch already exists?
        // Parent branch exists?
    }

    void SemanticalVerifier::verifyInsert(SQLParserResult &result) {
        Database &db = _context.analyzingContext.db;
        InsertStatement* stmt = result.insertStmt;
        if (stmt == nullptr) throw semantic_sql_error("unknown statement type");

        if (!db.hasTable(stmt->relation.name)) throw semantic_sql_error("table '" + stmt->relation.name + "' does not exist");
        if (db._branchMapping.find(stmt->relation.version) == db._branchMapping.end()) throw semantic_sql_error("version '" + stmt->relation.version + "' does not exist");
        Table *table = db.getTable(stmt->relation.name);
        std::vector<std::string> columnNames = table->getColumnNames();
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

    void SemanticalVerifier::verifySelect(SQLParserResult &result) {
        Database &db = _context.analyzingContext.db;
        SelectStatement* stmt = result.selectStmt;
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

    void SemanticalVerifier::verifyUpdate(SQLParserResult &result) {
        Database &db = _context.analyzingContext.db;
        UpdateStatement* stmt = result.updateStmt;
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

    void SemanticalVerifier::verifyDelete(SQLParserResult &result) {
        Database &db = _context.analyzingContext.db;
        DeleteStatement* stmt = result.deleteStmt;
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

    void SemanticalVerifier::analyse_sql_statement(SQLParserResult &result) {
        switch (result.opType) {
            case SQLParserResult::OpType::CreateTable:
                verifyCreateTable(result);
                break;
            case SQLParserResult::OpType::CreateBranch:
                verifyCreateBranch(result);
                break;
            case SQLParserResult::OpType::Insert:
                verifyInsert(result);
                break;
            case SQLParserResult::OpType::Select:
                verifySelect(result);
                break;
            case SQLParserResult::OpType::Update:
                verifyUpdate(result);
                break;
            case SQLParserResult::OpType::Delete:
                verifyDelete(result);
                break;
        }
    }

}