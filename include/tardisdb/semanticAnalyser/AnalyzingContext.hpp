//
// Created by Blum Thomas on 14.10.20.
//

#ifndef PROTODB_ANALYZINGCONTEXT_HPP
#define PROTODB_ANALYZINGCONTEXT_HPP

#include "foundations/Database.hpp"
#include "semanticAnalyser/ParserResult.hpp"
#include "foundations/IUFactory.hpp"
#include "semanticAnalyser/JoinGraph.hpp"
#include "algebra/logical/operators.hpp"

namespace semanticalAnalysis {
    struct AnalyzingContext {
        SQLParserResult parserResult;
        Database & db;
        IUFactory iuFactory;

        using scope_op_t = std::unique_ptr<std::unordered_map<std::string, iu_p_t>>;
        std::unordered_map<std::string, iu_p_t> scope;

        void* callback = nullptr;

        JoinGraph graph;
        std::set<branch_id_t> branchIds;

        /// per production, the columns by name: those of the scanned branch, and tid
        std::unordered_map<std::string,std::unordered_map<std::string, iu_p_t>> ius;
        /// per production, everything its scan produces: all columns of the table, which an update writes
        std::unordered_map<std::string, std::vector<iu_p_t>> scanIUs;
        std::unordered_map<std::string, std::unique_ptr<Operator>> dangling_productions;

        std::unique_ptr<Operator> joinedTree;

        /// of statements without a plan: the tuples they changed, and a message for the client
        uint64_t changedTuples = 0;
        std::string notice;

        AnalyzingContext(Database &db) : db(db) {}
        ~AnalyzingContext() {
            callback = nullptr;
        }

        iu_p_t getUniqueColumnIU(std::string columnName) {
            for (auto& production : ius) {
                for (auto &iu : production.second) {
                    if (iu.first.compare(columnName) == 0) {
                        return iu.second;
                    }
                }
            }
            return nullptr;
        }
    };
}


#endif //PROTODB_ANALYZINGCONTEXT_HPP
