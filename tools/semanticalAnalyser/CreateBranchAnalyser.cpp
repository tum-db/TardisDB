//
// Created by Blum Thomas on 2020-06-29.
//

#include "semanticAnalyser/SemanticAnalyser.hpp"

namespace semanticalAnalysis {

    void CreateBranchAnalyser::verify() {
        Database &db = _context.db;
        CreateBranchStatement* stmt = _context.parserResult.createBranchStmt;
        if (stmt == nullptr) throw semantic_sql_error("unknown statement type");

        if (db._branchMapping.find(stmt->branchName) != db._branchMapping.end())
            throw semantic_sql_error("branch '" + stmt->branchName + "' already exists");
        if (db._branchMapping.find(stmt->parentBranchName) == db._branchMapping.end())
            throw semantic_sql_error("parent branch '" + stmt->parentBranchName + "' does not exist");
        // Branch already exists?
        // Parent branch exists?
    }

    std::unique_ptr<Operator> CreateBranchAnalyser::constructTree() {
        QueryPlan plan;

        // Get branchName and parentBranchId from statement
        std::string &branchName = _context.parserResult.createBranchStmt->branchName;
        std::string &parentBranchName = _context.parserResult.createBranchStmt->parentBranchName;

        // Search for mapped branchId of parentBranchName
        branch_id_t _parentBranchId = 0;
        if (parentBranchName.compare("master") != 0) {
            _parentBranchId = _context.db._branchMapping[parentBranchName];
        }

        // Add new branch
        branch_id_t branchid = _context.db.createBranch(branchName, _parentBranchId);

        std::cout << "Created Branch " << branchid << "\n";

        return nullptr;
    }

}