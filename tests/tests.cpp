
#include "tests.hpp"

#include <cstdlib>
#include <functional>
#include <unordered_map>

using testFunc = std::function<void ()>;

static std::unordered_map<std::string, testFunc> tests = {
        { "expression", &expression_test },
        { "groupby", &groupby_test }
};

void runNamedTest(const std::string & testName)
{
    auto it = tests.find(testName);
    if (it != tests.end()) {
        it->second();
    } else {
        fprintf(stderr, "test '%s' not found.\n", testName.c_str());
    }
}
