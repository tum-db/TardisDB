//
// Created by Blum Thomas on 2020-06-29.
//

#include "semanticAnalyser/SemanticAnalyser.hpp"

namespace semanticalAnalysis {

    //TODO: Verifier check on only one table to update
    std::unique_ptr<Operator> UpdateAnalyser::constructTree() {
        QueryPlan plan;

        construct_scans(_context, plan, _parserResult);
        construct_selects(_context, plan, _parserResult);

        std::string &relationName = _parserResult.relations[0].first;
        Table* table = _context.db.getTable(relationName);

        std::vector<std::pair<iu_p_t,std::string>> updateIUs;

        //Get all ius of the tuple to update
        for (auto& production : plan.ius) {
            for (auto &iu : production.second) {
                updateIUs.emplace_back( iu.second, "" );
            }
        }

        //Map values to be updated to the corresponding ius
        for (auto columnValuePairs : _parserResult.columnToValue) {
            const std::string &valueString = columnValuePairs.second;

            for (auto &iuPair : updateIUs) {
                if (iuPair.first->columnInformation->columnName.compare(columnValuePairs.first) == 0) {
                    iuPair.second = valueString;
                }
            }
        }

        auto &production = plan.dangling_productions[relationName];

        return std::make_unique<Update>( std::move(production), updateIUs, *table, new std::string(relationName));
    }

}