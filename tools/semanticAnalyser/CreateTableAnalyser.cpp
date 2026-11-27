//
// Created by Blum Thomas on 2020-06-29.
//

#include "semanticAnalyser/SemanticAnalyser.hpp"

#include <map>

namespace semanticalAnalysis {

    void CreateTableAnalyser::verify() {
        Database &db = _context.db;
        CreateTableStatement* stmt = _context.parserResult.createTableStmt;

        if (stmt == nullptr) throw semantic_sql_error("unknown statement type");
        if (db.hasTable(stmt->tableName)) {
            if (stmt->ifNotExists) return;
            throw semantic_sql_error("table '" + stmt->tableName + "' already exists");
        }
        std::vector<std::string> definedColumnNames;
        for (auto &column : stmt->columns) {
            if (std::find(definedColumnNames.begin(),definedColumnNames.end(),column.name) != definedColumnNames.end())
                throw semantic_sql_error("column '" + column.name + "' already exists");
            definedColumnNames.push_back(column.name);
            if (!isColumnType(column.type))
                throw semantic_sql_error("type '" + column.type + "' does not exist");
        }

        for (auto &key : stmt->primaryKey) {
            if (std::find(definedColumnNames.begin(), definedColumnNames.end(), key) == definedColumnNames.end())
                throw semantic_sql_error("primary key column '" + key + "' does not exist");
        }

        // Table already exists?
        // For each columnspec
        // // name already exists?
        // // type exists?
    }

    void CreateTableAnalyser::constructTree() {
        CreateTableStatement *stmt = _context.parserResult.createTableStmt;
        if (stmt->ifNotExists && _context.db.hasTable(stmt->tableName)) {
            _context.joinedTree = nullptr;
            return;
        }
        auto & createdTable = _context.db.createTable(stmt->tableName);

        for (auto &columnSpec : stmt->columns) {
            // a column without NOT NULL (or PRIMARY KEY) takes NULL values
            createdTable.addColumn(columnSpec.name, toColumnType(columnSpec));
        }
        createdTable.setPrimaryKey(stmt->primaryKey);

        _context.joinedTree = nullptr;
    }

    /// The type without the suffix of an array type ("integer[]": "integer")
    static std::string elementTypeName(const std::string &type, bool &array) {
        array = type.size() > 2 && type.compare(type.size() - 2, 2, "[]") == 0;
        return array ? type.substr(0, type.size() - 2) : type;
    }

    bool isColumnType(const std::string &spelled) {
        static const std::vector<std::string> typeNames = {"bool","date","integer","int","longinteger","numeric","char","varchar","timestamp","text",
                // names of PostgreSQL
                "boolean","smallint","int2","int4","bigint","int8","decimal","float","float4","float8","real","double","bpchar","character"};
        bool array;
        std::string type = elementTypeName(spelled, array);
        return std::find(typeNames.begin(), typeNames.end(), type) != typeNames.end();
    }

    Sql::SqlType toColumnType(const ColumnSpec &spec) {
        // the names of PostgreSQL map to the types of TardisDB; float, real and double (precision) are
        // double precision; a type with [] (or ARRAY) is an array of the type
        ColumnSpec columnSpec = spec;
        bool array;
        columnSpec.type = elementTypeName(spec.type, array);
        if (array) {
            ColumnSpec element = columnSpec;
            element.nullable = false;
            return Sql::getArrayTy(toColumnType(element), spec.nullable);
        }
        static const std::map<std::string, std::string> aliases = {
                {"boolean", "bool"}, {"smallint", "integer"}, {"int2", "integer"}, {"int4", "integer"},
                {"bigint", "longinteger"}, {"int8", "longinteger"}, {"decimal", "numeric"}, {"bpchar", "char"},
                {"character", "char"}};
        if (aliases.count(columnSpec.type)) columnSpec.type = aliases.at(columnSpec.type);
        if (columnSpec.type == "char" && columnSpec.length == 0) columnSpec.length = 1; // char is char(1)
        Sql::SqlType sqlType;
        if (columnSpec.type == "float" || columnSpec.type == "float4" || columnSpec.type == "float8" ||
            columnSpec.type == "real" || columnSpec.type == "double") {
            return Sql::getDoubleTy(columnSpec.nullable);
        }
        if (columnSpec.type.compare("bool") == 0) {
            sqlType = Sql::getBoolTy(columnSpec.nullable);
        } else if (columnSpec.type.compare("date") == 0) {
            sqlType = Sql::getDateTy(columnSpec.nullable);
        } else if (columnSpec.type.compare("integer") == 0) {
            sqlType = Sql::getIntegerTy(columnSpec.nullable);
        } else if (columnSpec.type.compare("int") == 0) {
            sqlType = Sql::getIntegerTy(columnSpec.nullable);
        } else if (columnSpec.type.compare("longinteger") == 0) {
            sqlType = Sql::getLongIntegerTy(columnSpec.nullable);
        } else if (columnSpec.type.compare("numeric") == 0) {
            sqlType = Sql::getNumericTy(columnSpec.length,columnSpec.precision,columnSpec.nullable);
        } else if (columnSpec.type.compare("char") == 0) {
            sqlType = Sql::getCharTy(columnSpec.length, columnSpec.nullable);
        } else if (columnSpec.type.compare("varchar") == 0) {
            sqlType = Sql::getVarcharTy(columnSpec.length, columnSpec.nullable);
        } else if (columnSpec.type.compare("timestamp") == 0) {
            sqlType = Sql::getTimestampTy(columnSpec.nullable);
        } else if (columnSpec.type.compare("text") == 0) {
            sqlType = Sql::getTextTy(columnSpec.nullable);
        }
        return sqlType;
    }

}