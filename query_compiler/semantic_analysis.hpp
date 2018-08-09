
#ifndef SEMANTIC_ANALYSIS_HPP
#define SEMANTIC_ANALYSIS_HPP

#include <string>

#include "algebra/Operator.hpp"

#include "queryparser.hpp"

/// \brief Build the algebra tree of the given query
std::unique_ptr<Algebra::Operator> computeTree(const QueryParser::ParsedQuery & query);

#endif // SEMANTIC_ANALYSIS_HPP
