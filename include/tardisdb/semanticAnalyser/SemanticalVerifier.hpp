//
// Created by Blum Thomas on 2020-06-29.
//

#ifndef PROTODB_SEMANTICALVERIFIER_HPP
#define PROTODB_SEMANTICALVERIFIER_HPP

#include "semanticAnalyser/ParserResult.hpp"
#include "foundations/Database.hpp"
#include "queryCompiler/QueryContext.hpp"
#include "algebra/logical/operators.hpp"

namespace semanticalAnalysis {


    void addToScope(QueryContext & context, iu_p_t iu, const std::string & symbol);

    void addToScope(QueryContext & context, Algebra::Logical::TableScan & scan);

    void addToScope(QueryContext & context, Algebra::Logical::TableScan & scan, const std::string & prefix);

    iu_p_t lookup(QueryContext & context, const std::string & symbol);
}

#endif //PROTODB_SEMANTICALVERIFIER_HPP
