//
// Created by Blum Thomas on 2020-06-29.
//

#include "semanticAnalyser/SemanticAnalyser.hpp"

namespace semanticalAnalysis {

    //TODO: Make projections available to every node in the tree
//TODO: Check physical tree projections do not work after joining
    std::unique_ptr<Operator> SelectAnalyser::constructTree() {
        QueryPlan plan;

        construct_scans(_context, plan, _parserResult);
        construct_selects(_context, plan, _parserResult);
        construct_joins(_context,plan, _parserResult);

        auto & db = _context.db;

        //get projected IUs
        std::vector<iu_p_t> projectedIUs;
        for (const std::string& projectedIUName : _parserResult.projections) {
            for (auto& production : plan.ius) {
                for (auto& iu : production.second) {
                    if (iu.first.compare(projectedIUName) == 0) {
                        projectedIUs.push_back( iu.second );
                    }
                }
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