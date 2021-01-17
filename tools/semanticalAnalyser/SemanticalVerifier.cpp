//
// Created by Blum Thomas on 2020-06-29.
//

#include "semanticAnalyser/SemanticalVerifier.hpp"
#include "foundations/exceptions.hpp"
#include "algebra/logical/operators.hpp"

namespace semanticalAnalysis {

    void addToScope(QueryContext & context, iu_p_t iu, const std::string & symbol)
    {
        context.analyzingContext.scope.emplace(symbol, iu);
    }

    void addToScope(QueryContext & context, Algebra::Logical::TableScan & scan)
    {
        auto & scope = context.analyzingContext.scope;
        for (iu_p_t iu : scan.getProduced()) {
            ci_p_t ci = getColumnInformation(iu);
            scope.emplace(ci->columnName, iu);
        }
    }

    void addToScope(QueryContext & context, Algebra::Logical::TableScan & scan, const std::string & prefix)
    {
        auto & scope = context.analyzingContext.scope;
        for (iu_p_t iu : scan.getProduced()) {
            ci_p_t ci = getColumnInformation(iu);
            scope.emplace(prefix + "." + ci->columnName, iu);
        }
    }

    iu_p_t lookup(QueryContext & context, const std::string & symbol)
    {
        auto & scope = context.analyzingContext.scope;
        auto it = scope.find(symbol);
        if (it == scope.end()) {
            throw ParserException("unknown identifier: " + symbol);
        }
        return it->second;
    }

}