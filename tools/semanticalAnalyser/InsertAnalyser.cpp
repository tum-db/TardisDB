//
// Created by Blum Thomas on 2020-06-29.
//

#include "semanticAnalyser/SemanticAnalyser.hpp"

namespace semanticalAnalysis {

    std::unique_ptr<Operator> InsertAnalyser::constructTree() {
        QueryPlan plan;
        plan.parser_result = _parserResult;

        auto& db = _context.db;
        Table* table = db.getTable(plan.parser_result.relation);

        if (plan.parser_result.versions.size() == 1) {
            std::string &branchName = plan.parser_result.versions[0];
            if (branchName.compare("master") != 0) {
                _context.executionContext.branchId = db._branchMapping[branchName];
            } else {
                _context.executionContext.branchId = 0;
            }
        } else {
            return nullptr;
        }

        std::vector<std::unique_ptr<Native::Sql::Value>> sqlvalues;

        for (int i=0; i<plan.parser_result.columnNames.size(); i++) {
            Native::Sql::SqlType type = table->getCI(plan.parser_result.columnNames[i])->type;
            std::string &value = plan.parser_result.values[i];
            std::unique_ptr<Native::Sql::Value> sqlvalue = Native::Sql::Value::castString(value,type);

            sqlvalues.emplace_back(std::move(sqlvalue));
        }

        Native::Sql::SqlTuple *tuple =  new Native::Sql::SqlTuple(std::move(sqlvalues));

        plan.tree = std::make_unique<Insert>(_context,*table,tuple);

        return std::move(plan.tree);
    }

}