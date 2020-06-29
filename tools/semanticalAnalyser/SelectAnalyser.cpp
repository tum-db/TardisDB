//
// Created by Blum Thomas on 2020-06-29.
//

#include "semanticAnalyser/SemanticAnalyser.hpp"

namespace semanticalAnalysis {

    void SelectAnalyser::constructTree(QueryPlan& plan) {
        construct_scans(_context, plan);
        construct_selects(_context, plan);
        construct_joins(_context,plan);
        construct_projection(_context, plan);
    }

}