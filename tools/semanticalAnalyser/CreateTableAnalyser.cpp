//
// Created by Blum Thomas on 2020-06-29.
//

#include "semanticAnalyser/SemanticAnalyser.hpp"

namespace semanticalAnalysis {

    void CreateTableAnalyser::constructTree(QueryPlan& plan) {
        auto & createdTable = _context.db.createTable(plan.parser_result.relation);

        for (int i = 0; i < plan.parser_result.columnNames.size(); i++) {
            std::string &columnName = plan.parser_result.columnNames[i];
            std::string &columnType = plan.parser_result.columnTypes[i];
            bool nullable = plan.parser_result.nullable[i];
            uint32_t length = plan.parser_result.length[i];
            uint32_t precision = plan.parser_result.precision[i];

            Sql::SqlType sqlType;
            if (columnType.compare("bool") == 0) {
                sqlType = Sql::getBoolTy(nullable);
            } else if (columnType.compare("date") == 0) {
                sqlType = Sql::getDateTy(nullable);
            } else if (columnType.compare("integer") == 0) {
                sqlType = Sql::getIntegerTy(nullable);
            } else if (columnType.compare("longinteger") == 0) {
                sqlType = Sql::getLongIntegerTy(nullable);
            } else if (columnType.compare("numeric") == 0) {
                sqlType = Sql::getNumericTy(length,precision,nullable);
            } else if (columnType.compare("char") == 0) {
                sqlType = Sql::getCharTy(length, nullable);
            } else if (columnType.compare("varchar") == 0) {
                sqlType = Sql::getVarcharTy(length, nullable);
            } else if (columnType.compare("timestamp") == 0) {
                sqlType = Sql::getTimestampTy(nullable);
            } else if (columnType.compare("text") == 0) {
                sqlType = Sql::getTextTy(nullable);
            }

            createdTable.addColumn(columnName, sqlType);
        }
    }

}