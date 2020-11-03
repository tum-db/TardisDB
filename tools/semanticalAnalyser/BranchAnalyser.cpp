//
// Created by Blum Thomas on 2020-06-29.
//

#include "semanticAnalyser/SemanticAnalyser.hpp"

namespace semanticalAnalysis {

    void BranchAnalyser::verify() {
        // No assertions to be done
    }

    void BranchAnalyser::constructTree() {
        std::vector<std::pair<std::string,std::string>> derivations;
        for (auto &[_,branch] : _context.db._branches) {
            if (branch->id == 0) continue;
            auto &parentBranch = _context.db._branches[branch->parent_id];
            derivations.push_back(std::make_pair(branch->name,parentBranch->name));
        }

        std::cout << "{";
        int derivationsCount = derivations.size();
        for (int i=0; i<derivationsCount-1; i++) {
            std::cout << "[\"" << derivations[i].first << "\",\"" << derivations[i].second << "\"]," ;
        }
        if (derivationsCount > 0) std::cout << "[\"" << derivations[derivationsCount-1].first << "\",\"" << derivations[derivationsCount-1].second << "\"]";
        std::cout << "}" << std::endl ;

        _context.joinedTree = nullptr;
    }

}