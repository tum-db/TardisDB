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

#include <memory>

namespace semanticalAnalysis {
    class CopyWriter;

    struct AnalyzingContext {
        SQLParserResult parserResult;
        Database & db;
        IUFactory iuFactory;

        using scope_op_t = std::unique_ptr<std::unordered_map<std::string, iu_p_t>>;
        std::unordered_map<std::string, iu_p_t> scope;

        /// COPY ... TO: writes the result tuples into the file instead of returning them
        std::shared_ptr<CopyWriter> copyWriter;

        /// INSERT ... SELECT: the types of the columns of the select list, which a NULL without a type takes (otherwise text)
        std::vector<Sql::SqlType> projectionTypes;

        JoinGraph graph;
        std::set<branch_id_t> branchIds;

        /// per production, the columns by name: those of the scanned branch, and tid
        std::unordered_map<std::string,std::unordered_map<std::string, iu_p_t>> ius;
        /// per production, everything its scan produces: all columns of the table, which an update writes
        std::unordered_map<std::string, std::vector<iu_p_t>> scanIUs;
        std::unordered_map<std::string, std::unique_ptr<Operator>> dangling_productions;

        std::unique_ptr<Operator> joinedTree;

        /// conditions of WHERE on the columns of several tables, which apply to the joined tree
        std::vector<Expressions::exp_op_t> joinedPredicates;

        /// the plans of the subqueries in FROM, by the names of their relations, and their contexts (which own their IUs)
        struct DerivedTable {
            std::vector<std::string> names;
            std::vector<iu_p_t> ius;
            std::unique_ptr<Algebra::Logical::Operator> tree;
        };
        std::unordered_map<std::string, DerivedTable> derivedTables;
        std::vector<std::unique_ptr<AnalyzingContext>> nested;

        /// of statements without a plan: the tuples they changed, and a message for the client
        uint64_t changedTuples = 0;
        std::string notice;

        AnalyzingContext(Database &db) : db(db) {}

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
