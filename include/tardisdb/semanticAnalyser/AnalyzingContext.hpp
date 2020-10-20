//
// Created by Blum Thomas on 14.10.20.
//

#ifndef PROTODB_ANALYZINGCONTEXT_HPP
#define PROTODB_ANALYZINGCONTEXT_HPP

#include "foundations/Database.hpp"
#include "semanticAnalyser/ParserResult.hpp"
#include "semanticAnalyser/logicalAlgebra/IUFactory.hpp"

namespace semanticalAnalysis {
    struct AnalyzingContext {
        SQLParserResult parserResult;
        Database & db;
        IUFactory iuFactory;
        uint32_t operatorUID = 0;

        using scope_op_t = std::unique_ptr<std::unordered_map<std::string, iu_p_t>>;
        std::unordered_map<std::string, iu_p_t> scope;

        AnalyzingContext(Database &db) : db(db) {}
    };
}


#endif //PROTODB_ANALYZINGCONTEXT_HPP
