//
// Created by Blum Thomas on 2020-06-29.
//

#ifndef PROTODB_SEMANTICALVERIFIER_HPP
#define PROTODB_SEMANTICALVERIFIER_HPP

#include "sqlParser/ParserResult.hpp"
#include "foundations/Database.hpp"
#include "foundations/QueryContext.hpp"

namespace semanticalAnalysis {
    struct semantic_sql_error : std::runtime_error {
        //semantic or syntactic errors
        using std::runtime_error::runtime_error;
    };

    class SemanticalVerifier {
    public:
        SemanticalVerifier(QueryContext &context) : _context(context) {}
        void analyse_sql_statement(tardisParser::SQLParserResult &result);
    private:
        QueryContext& _context;
    };
}



#endif //PROTODB_SEMANTICALVERIFIER_HPP
