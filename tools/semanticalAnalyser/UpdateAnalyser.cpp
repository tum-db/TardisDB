//
// Created by Blum Thomas on 2020-06-29.
//

#include "semanticAnalyser/SemanticAnalyser.hpp"

namespace semanticalAnalysis {

    //TODO: Verifier check on only one table to update
    std::unique_ptr<Operator> UpdateAnalyser::constructTree() {
        QueryPlan plan;
        UpdateStatement *stmt = _parserResult.updateStmt;

        std::vector<Relation> relations;
        relations.push_back(stmt->relation);
        construct_scans(_context, plan, relations);
        construct_selects(plan, stmt->selections);

        Table* table = _context.db.getTable(stmt->relation.name);
        if (stmt->relation.alias.length() == 0) stmt->relation.alias = stmt->relation.name;

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

        return std::make_unique<Update>( std::move(production), updateIUs, *table, new std::string(stmt->relation.name));
    }

}