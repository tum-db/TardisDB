
#include "queries/queries.hpp"

#include <cstdlib>
#include <functional>
#include <unordered_map>

#include "foundations/QueryContext.hpp"

using benchmarkFunc = std::function<void (unsigned)>;

static std::unordered_map<std::string, benchmarkFunc> benchmarks = {
        { "tpch1", &benchmark_tpc_h_1 },
        { "tpch1-null", &benchmark_tpc_h_1_null },
        { "arithmetic1-null", &benchmark_arithmetic_1_null },
        { "arithmetic2-null", &benchmark_arithmetic_2_null },
        { "arithmetic3-null", &benchmark_arithmetic_3_null },
        { "arithmetic4-null", &benchmark_arithmetic_4_null }
};

void benchmarkNamedQuery(const std::string & queryName, unsigned runs)
{
    auto it = benchmarks.find(queryName);
    if (it != benchmarks.end()) {
        it->second(runs);
        if (overflowFlag) {
            fprintf(stderr, "arithmetic overflow\n");
        }
    } else {
        fprintf(stderr, "query '%s' not found.\n", queryName.c_str());
    }
}

using queryFunc = std::function<void ()>;

static std::unordered_map<std::string, queryFunc> queries = {
        { "tpch1", &run_tpc_h_1 },
        { "tpch1-null", &run_tpc_h_1_null }
};

void runNamedQuery(const std::string & queryName)
{
    auto it = queries.find(queryName);
    if (it != queries.end()) {
        it->second();
        if (overflowFlag) {
            fprintf(stderr, "arithmetic overflow\n");
        }
    } else {
        fprintf(stderr, "query '%s' not found.\n", queryName.c_str());
    }
}
