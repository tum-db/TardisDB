
#include "queryCompiler/QueryContext.hpp"

#include <llvm/IR/TypeBuilder.h>

#include "foundations/exceptions.hpp"
#include "algebra/logical/operators.hpp"

void ExecutionContext::acquireResource(std::unique_ptr<ExecutionResource> && resource)
{
    resources.push_back(std::move(resource));
}

void addToScope(QueryContext & context, iu_p_t iu, const std::string & symbol)
{
    context.scope.emplace(symbol, iu);
}

void addToScope(QueryContext & context, Algebra::Logical::TableScan & scan)
{
    auto & scope = context.scope;
    for (iu_p_t iu : scan.getProduced()) {
        ci_p_t ci = getColumnInformation(iu);
        scope.emplace(ci->columnName, iu);
    }
}

void addToScope(QueryContext & context, Algebra::Logical::TableScan & scan, const std::string & prefix)
{
    auto & scope = context.scope;
    for (iu_p_t iu : scan.getProduced()) {
        ci_p_t ci = getColumnInformation(iu);
        scope.emplace(prefix + "." + ci->columnName, iu);
    }
}

iu_p_t lookup(QueryContext & context, const std::string & symbol)
{
    auto & scope = context.scope;
    auto it = scope.find(symbol);
    if (it == scope.end()) {
        throw ParserException("unknown identifier: " + symbol);
    }
    return it->second;
}
