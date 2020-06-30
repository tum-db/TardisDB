//
// Created by Blum Thomas on 2020-06-29.
//

#include "semanticAnalyser/SemanticAnalyser.hpp"

namespace semanticalAnalysis {

    std::unique_ptr<Operator> DeleteAnalyser::constructTree() {
        QueryPlan plan;

        construct_scans(_context, plan, _parserResult);
        construct_selects(_context, plan, _parserResult);

        if (plan.dangling_productions.size() != 1 || _parserResult.relations.size() != 1) {
            throw std::runtime_error("no or more than one root found: Table joining has failed");
        }

        auto &relationName = _parserResult.relations[0].second;
        if (relationName.length() == 0) relationName = _parserResult.relations[0].first;
        Table* table = _context.db.getTable(_parserResult.relations[0].first);

        iu_p_t tidIU;

        for (auto& production : plan.ius) {
            for (auto &iu : production.second) {
                if (iu.first.compare("tid") == 0) {
                    tidIU = iu.second;
                    break;
                }
            }
        }

        auto &production = plan.dangling_productions[relationName];

        return std::make_unique<Delete>( std::move(production), tidIU, *table);
    }

}
