//
// Created by Blum Thomas on 2020-06-29.
//

#include "semanticAnalyser/SemanticAnalyser.hpp"

namespace semanticalAnalysis {

    std::unique_ptr<Operator> SelectAnalyser::constructTree() {
        QueryPlan plan;
        plan.parser_result = _parserResult;

        construct_scans(_context, plan);
        construct_selects(_context, plan);
        construct_joins(_context,plan);
        construct_projection(_context, plan);

        return std::move(plan.tree);
    }

}