
#ifndef SEMANTIC_ANALYSIS_HPP
#define SEMANTIC_ANALYSIS_HPP

#include <string>

#include "algebra/logical/operators.hpp"
#include "query_compiler/queryparser.hpp"

/// \brief Build a algebra tree for the given query
std::unique_ptr<Algebra::Logical::Operator> computeTree(const QueryParser::ParsedQuery & query);

#endif // SEMANTIC_ANALYSIS_HPP
