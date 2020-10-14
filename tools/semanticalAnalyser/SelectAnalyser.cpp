//
// Created by Blum Thomas on 2020-06-29.
//

#include "semanticAnalyser/SemanticAnalyser.hpp"

namespace semanticalAnalysis {

    //TODO: Make projections available to every node in the tree
//TODO: Check physical tree projections do not work after joining
    std::unique_ptr<Operator> SelectAnalyser::constructTree() {
        QueryPlan plan;
        SelectStatement *stmt = _parserResult.selectStmt;

        construct_scans(_context, plan, stmt->relations);
        construct_selects(plan, stmt->selections);
        construct_joins(_context,plan, _parserResult);

        auto & db = _context.db;

        //get projected IUs
        std::vector<iu_p_t> projectedIUs;
        for (auto &column : stmt->projections) {
            if (column.table.length() == 0) {
                for (auto& production : plan.ius) {
                    for (auto& iu : production.second) {
                        if (iu.first.compare(column.name) == 0) {
                            projectedIUs.push_back( iu.second );
                        }
                    }
                }
            } else {
                projectedIUs.push_back(plan.ius[column.table][column.name]);
            }
        }

        if (plan.joinedTree != nullptr) {
            // Construct Result and store it in the query plan struct
            return std::make_unique<Result>( std::move(plan.joinedTree), projectedIUs );
        } else {
            throw std::runtime_error("no or more than one root found: Table joining has failed");
        }
    }

}