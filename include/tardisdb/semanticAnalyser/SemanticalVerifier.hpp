//
// Created by Blum Thomas on 2020-06-29.
//

#ifndef PROTODB_SEMANTICALVERIFIER_HPP
#define PROTODB_SEMANTICALVERIFIER_HPP

#include "semanticAnalyser/ParserResult.hpp"
#include "foundations/Database.hpp"
#include "queryCompiler/QueryContext.hpp"

namespace semanticalAnalysis {
    struct semantic_sql_error : std::runtime_error {
        //semantic or syntactic errors
        using std::runtime_error::runtime_error;
    };

    class SemanticalVerifier {
    public:
        SemanticalVerifier(QueryContext &context) : _context(context) {}
        void analyse_sql_statement(SQLParserResult &result);
    private:
        QueryContext& _context;
    };

    void addToScope(QueryContext & context, iu_p_t iu, const std::string & symbol);

    void addToScope(QueryContext & context, Algebra::Logical::TableScan & scan);

    void addToScope(QueryContext & context, Algebra::Logical::TableScan & scan, const std::string & prefix);

    iu_p_t lookup(QueryContext & context, const std::string & symbol);
}



#endif //PROTODB_SEMANTICALVERIFIER_HPP
