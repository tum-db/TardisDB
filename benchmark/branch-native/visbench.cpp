// Visibility test inside a version-chain walk: TardisDB's lineage map vs. interval labels.
//
// Workload as in branchBench --mode=depth: n tuples, a chain of branches
// main -> b1 -> ... -> bD; after each fork, the new branch and main each update
// a fraction of random tuples. A reader in bD walks every chain (newest first)
// to the first visible version.
//
// lineage:  as in TardisDB (version_management.cpp, is_visible): a version stores
//           (branch, ts); it is visible if branch == reader, or the lineage map
//           (parent -> fork time of the child on the path) holds branch and ts < fork time
// interval: as proposed: a fork gives the parent and the child a new identifier;
//           a version stores the identifier it was written under and is visible
//           if that identifier's label [l, r] contains the reader's label
//
// Both tests are checked to select the same version for every tuple.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

struct Version {
    uint32_t branch;   // lineage: creating branch
    uint32_t ts;       // lineage: creation time
    uint32_t ident;    // interval: identifier written under
    uint32_t payload;
    Version * next;
};

struct IdentNode { std::vector<uint32_t> children; };

int main(int argc, char ** argv) {
    const uint64_t n = argc > 1 ? std::stoull(argv[1]) : 1000000;
    const double frac = argc > 2 ? std::stod(argv[2]) : 0.01;
    const int runs = argc > 3 ? std::stoi(argv[3]) : 5;
    const std::vector<uint32_t> depths = {0, 1, 2, 4, 8, 16, 32, 64, 128};

    std::printf("depth,n,frac,lineage_ms,interval_ms,lineage_ns_per_tuple,interval_ns_per_tuple,avg_chain_walk\n");
    for (uint32_t depth : depths) {
        std::mt19937_64 rng(42);
        uint32_t now = 0;
        // branches: 0 = main, i = b_i; identifiers form a tree
        std::vector<uint32_t> parent(depth + 1, UINT32_MAX), forkTime(depth + 1, 0), current(depth + 1, 0);
        std::vector<IdentNode> idents(1);   // identifier 0 = main^0
        auto newIdent = [&](uint32_t of) { idents.emplace_back(); idents[of].children.push_back(idents.size() - 1); return uint32_t(idents.size() - 1); };

        std::vector<Version *> head(n);
        auto write = [&](uint64_t tid, uint32_t branch) {
            Version * v = static_cast<Version *>(std::malloc(sizeof(Version)));
            *v = Version{branch, now, current[branch], uint32_t(rng()), head[tid]};
            head[tid] = v;
        };
        for (uint64_t t = 0; t < n; ++t) write(t, 0);
        ++now;

        std::uniform_int_distribution<uint64_t> dist(0, n - 1);
        for (uint32_t b = 1; b <= depth; ++b) {
            // fork b from b-1: the parent continues under a new identifier, the child starts with one
            uint32_t p = b - 1;
            ++now;
            parent[b] = p;
            forkTime[b] = now;
            uint32_t old = current[p];
            current[p] = newIdent(old);
            current[b] = newIdent(old);
            ++now;
            for (uint64_t i = 0; i < uint64_t(frac * n); ++i) write(dist(rng), b);
            // main writes too: its updates are invisible to all deeper branches
            for (uint64_t i = 0; i < uint64_t(frac * n); ++i) write(dist(rng), 0);
            ++now;
        }

        // interval labels by DFS over the identifier tree (static, gaps irrelevant for the test cost)
        std::vector<uint32_t> L(idents.size()), R(idents.size());
        {
            uint32_t counter = 0;
            std::vector<std::pair<uint32_t, bool>> stack = {{0, false}};
            while (!stack.empty()) {
                auto [id, done] = stack.back(); stack.pop_back();
                if (done) { R[id] = counter++; continue; }
                L[id] = counter++;
                stack.push_back({id, true});
                for (auto it = idents[id].children.rbegin(); it != idents[id].children.rend(); ++it) stack.push_back({*it, false});
            }
        }

        const uint32_t reader = depth;
        // lineage map of the reader, as built by Database::constructBranchLineage
        std::unordered_map<uint32_t, uint32_t> lineage;
        for (uint32_t c = reader; parent[c] != UINT32_MAX; c = parent[c]) lineage[parent[c]] = forkTime[c];
        const uint32_t lq = L[current[reader]];

        auto visLineage = [&](const Version * v) {
            if (v->branch == reader) return true;
            auto it = lineage.find(v->branch);
            return it != lineage.end() && v->ts < it->second;
        };
        auto visInterval = [&](const Version * v) { return L[v->ident] <= lq && lq <= R[v->ident]; };

        // correctness and average walk length
        uint64_t steps = 0;
        for (uint64_t t = 0; t < n; ++t) {
            const Version * a = head[t]; while (a && !visLineage(a)) { a = a->next; ++steps; }
            const Version * b = head[t]; while (b && !visInterval(b)) b = b->next;
            if (a != b) { std::fprintf(stderr, "mismatch at depth %u tuple %lu\n", depth, t); return 1; }
        }

        auto scan = [&](auto vis) {
            std::vector<double> times;
            uint64_t sink = 0;
            for (int r = 0; r < runs; ++r) {
                auto start = std::chrono::steady_clock::now();
                for (uint64_t t = 0; t < n; ++t) {
                    const Version * v = head[t];
                    while (!vis(v)) v = v->next;
                    sink += v->payload;
                }
                times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            }
            std::sort(times.begin(), times.end());
            if (sink == 42) std::printf("#");
            return times[times.size() / 2];
        };
        double tl = scan(visLineage), ti = scan(visInterval);
        std::printf("%u,%lu,%g,%.3f,%.3f,%.2f,%.2f,%.3f\n", depth, n, frac, tl, ti, tl * 1e6 / n, ti * 1e6 / n, double(steps) / n);
        std::fflush(stdout);
        for (auto v : head) while (v) { auto nx = v->next; std::free(v); v = nx; }
    }
    return 0;
}
