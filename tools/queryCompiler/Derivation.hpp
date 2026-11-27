// derivation(TABLE (query), lambda (x) (expression)) in FROM: the rows of the query with the partial derivatives of the expression

#pragma once

#include <string>

#include "sqlParser/Expression.hpp"

namespace Derivation {

    /// \returns The SELECT of the rows of \p table with d_<c> for each column \p parameter.c that \p expression uses
    std::string query(const std::string & table, const std::string & parameter, const std::string & expression);

    /// \returns The expression as SQL, in parentheses wherever it has operators
    std::string toSql(const sqlExpressions::Node & node);

} // end namespace Derivation
