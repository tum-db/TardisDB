#include "queryCompiler/QueryContext.hpp"

void ExecutionContext::acquireResource(std::unique_ptr<ExecutionResource> && resource)
{
    resources.push_back(std::move(resource));
}
