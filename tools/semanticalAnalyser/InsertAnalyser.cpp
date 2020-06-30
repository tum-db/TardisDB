//
// Created by Blum Thomas on 2020-06-29.
//

#include "semanticAnalyser/SemanticAnalyser.hpp"

namespace semanticalAnalysis {

    std::unique_ptr<Operator> InsertAnalyser::constructTree() {
        QueryPlan plan;

        auto& db = _context.db;
        Table* table = db.getTable(_parserResult.relation);

        if (_parserResult.versions.size() == 1) {
            std::string &branchName = _parserResult.versions[0];
            if (branchName.compare("master") != 0) {
                _context.executionContext.branchId = db._branchMapping[branchName];
            } else {
                _context.executionContext.branchId = 0;
            }
        } else {
            return nullptr;
        }

        std::vector<std::unique_ptr<Native::Sql::Value>> sqlvalues;

        for (int i=0; i<_parserResult.columnNames.size(); i++) {
            Native::Sql::SqlType type = table->getCI(_parserResult.columnNames[i])->type;
            std::string &value = _parserResult.values[i];
            std::unique_ptr<Native::Sql::Value> sqlvalue = Native::Sql::Value::castString(value,type);

            sqlvalues.emplace_back(std::move(sqlvalue));
        }

        Native::Sql::SqlTuple *tuple =  new Native::Sql::SqlTuple(std::move(sqlvalues));

        return std::make_unique<Insert>(_context,*table,tuple);
    }

}