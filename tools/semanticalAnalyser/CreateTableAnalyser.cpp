//
// Created by Blum Thomas on 2020-06-29.
//

#include "semanticAnalyser/SemanticAnalyser.hpp"

namespace semanticalAnalysis {

    std::unique_ptr<Operator> CreateTableAnalyser::constructTree() {
        QueryPlan plan;

        auto & createdTable = _context.analyzingContext.db.createTable(_parserResult.createTableStmt->tableName);

        for (auto &columnSpec : _parserResult.createTableStmt->columns) {
            Sql::SqlType sqlType;
            if (columnSpec.type.compare("bool") == 0) {
                sqlType = Sql::getBoolTy(columnSpec.nullable);
            } else if (columnSpec.type.compare("date") == 0) {
                sqlType = Sql::getDateTy(columnSpec.nullable);
            } else if (columnSpec.type.compare("integer") == 0) {
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

            createdTable.addColumn(columnSpec.name, sqlType);
        }

        return nullptr;
    }

}