//
// Created by Blum Thomas on 2020-06-29.
//

#include "semanticAnalyser/SemanticAnalyser.hpp"

namespace semanticalAnalysis {

    void CreateBranchAnalyser::constructTree(QueryPlan& plan) {
        // Get branchName and parentBranchId from statement
        std::string &branchName = plan.parser_result.branchId;
        std::string &parentBranchName = plan.parser_result.parentBranchId;

        // Search for mapped branchId of parentBranchName
        branch_id_t _parentBranchId = 0;
        if (parentBranchName.compare("master") != 0) {
            _parentBranchId = _context.db._branchMapping[parentBranchName];
        }

        // Add new branch
        branch_id_t branchid = _context.db.createBranch(branchName, _parentBranchId);

        std::cout << "Created Branch " << branchid << "\n";
    }

}