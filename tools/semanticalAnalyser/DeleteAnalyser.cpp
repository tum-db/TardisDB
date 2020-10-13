//
// Created by Blum Thomas on 2020-06-29.
//

#include "semanticAnalyser/SemanticAnalyser.hpp"

namespace semanticalAnalysis {

    std::unique_ptr<Operator> DeleteAnalyser::constructTree() {
        QueryPlan plan;
        tardisParser::DeleteStatement *stmt = _parserResult.deleteStmt;

        std::vector<tardisParser::Table> relations;
        relations.push_back(stmt->relation);
        construct_scans(_context, plan, relations);
        construct_selects(plan, stmt->selections);

        if (stmt->relation.alias.length() == 0) stmt->relation.alias = stmt->relation.name;
        Table* table = _context.db.getTable(stmt->relation.name);

        iu_p_t tidIU;

        for (auto& production : plan.ius) {
            for (auto &iu : production.second) {
                if (iu.first.compare("tid") == 0) {
                    tidIU = iu.second;
                    break;
                }
            }
        }

        auto &production = plan.dangling_productions[stmt->relation.alias];

        return std::make_unique<Delete>( std::move(production), tidIU, *table);
    }

}
