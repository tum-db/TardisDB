// Branch microbenchmarks for TardisDB (baseline measurements for the DEFT paper).
//
//   depth: scan throughput in the leaf of a branch chain main -> b1 -> ... -> bD,
//          where every branch and main update a fraction of the tuples after each fork
//   fork:  latency of CREATE BRANCH over the table size
//   agentic: branch-mutate-evaluate loops of agents (topologies mcts, beam, bisect);
//           reports the time spent in fork, mutate and evaluate
//   crossbranch: k branches forked from main update a fraction of the tuples each; one
//           aggregate per branch is computed by k queries, by one query with k scans and by
//           one query with the branch-shared scan
//   concurrent: clients run statements at the same time, each in its own branch: agents (update, then
//           scan) and a reader beside a sibling branch that is written, read or inserted into, or beside
//           updates of main
//   verify: random forks, updates, inserts and deletes; every branch is scanned and
//           compared with a reference model in which a fork copies its parent's state;
//           aggregates over sets of branches are checked with both strategies
//
// Output is CSV on stdout.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <chrono>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/TargetSelect.h>

#include "algebra/logical/operators.hpp"
#include "foundations/Database.hpp"
#include "foundations/Parallel.hpp"
#include "foundations/version_management.hpp"
#include "queryCompiler/queryCompiler.hpp"
#include "sql/SqlValues.hpp"

#include "gflags/gflags.h"

DEFINE_string(mode, "depth", "depth | fork | agentic | crossbranch | diffmerge | cycles | concurrent | verify");
DEFINE_string(divergences, "10,100,1000,10000,100000", "tuples updated by the branch and by main (diffmerge mode)");
DEFINE_bool(gc, true, "collect versions no branch needs (DROP BRANCH) and overwrite versions of the current identifier");
DEFINE_bool(keep, false, "cycles mode: a branch forked from main before the cycles lives through them");
DEFINE_uint64(version_map, 0, "build the version map of a branch once this many scans of it were compiled (0: never)");
DEFINE_string(ks, "1,2,4,8,16,32,64,128,256,512,1024", "branch counts (crossbranch mode)");
DEFINE_string(aggregates, "sum(b)", "aggregates per branch (crossbranch mode)");
DEFINE_uint64(maxk_baseline, 256, "crossbranch mode: largest k for which the per-branch baselines run");
DEFINE_double(mainfrac, 0, "crossbranch mode: fraction of the tuples main updates after the forks");
DEFINE_uint64(ops, 3000, "operations (verify mode)");
DEFINE_string(topology, "mcts", "agentic mode: mcts | beam | bisect");
DEFINE_uint64(iterations, 200, "agentic mode: MCTS iterations, beam rounds or bisected snapshots");
DEFINE_uint64(width, 8, "agentic mode: children per node (mcts) or candidates per round (beam)");
DEFINE_uint64(mutations, 1000, "agentic mode: tuples updated per mutation");
DEFINE_uint64(churn, 0, "agentic mode: tuples deleted and tuples inserted per mutation, in addition to the updates");
DEFINE_bool(deep, false, "verify mode: always fork the newest branch, which builds deep chains");
DEFINE_uint64(threads, 0, "threads per query (0: all hardware threads; 1: serial code paths)");
DEFINE_uint64(morsel, 16384, "tuples per morsel");
DEFINE_bool(crossbranch, true, "verify mode: also check aggregates over sets of branches");
DEFINE_uint64(n, 1000000, "tuples");
DEFINE_string(depths, "0,1,2,4,8,16,32,64,128", "branch depths (depth mode)");
DEFINE_string(sizes, "10000,100000,1000000,10000000", "table sizes (fork mode)");
DEFINE_double(frac, 0.01, "fraction of tuples updated per branch and by main after each fork");
DEFINE_uint64(runs, 5, "repetitions per measurement (median reported)");
DEFINE_uint64(seed, 42, "random seed");
DEFINE_string(clients, "1,2,4,8,16", "concurrent mode: numbers of agents");
DEFINE_double(seconds, 3, "concurrent mode: duration of a measurement");
DEFINE_string(latching, "branch", "concurrent mode: serial (one statement at a time) | table (latches per table) | branch (and per branch)");

static void noop(Native::Sql::SqlTuple *) {}

static std::vector<uint64_t> parseList(const std::string & s) {
    std::vector<uint64_t> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) out.push_back(std::stoull(item));
    return out;
}

static double rssMiB() {
    std::ifstream statm("/proc/self/statm");
    size_t pages = 0, resident = 0;
    statm >> pages >> resident;
    return resident * 4096.0 / (1024.0 * 1024.0);
}

static Native::Sql::SqlTuple makeTuple(Table & table, uint64_t a, uint64_t b) {
    std::vector<std::unique_ptr<Native::Sql::Value>> values;
    values.push_back(Native::Sql::Value::castString(std::to_string(a), table.getCI("a")->type));
    values.push_back(Native::Sql::Value::castString(std::to_string(b), table.getCI("b")->type));
    return Native::Sql::SqlTuple(std::move(values));
}

static Table & load(Database & db, uint64_t n, uint64_t b = 0) {
    QueryCompiler::compileAndExecute("CREATE TABLE t ( a INTEGER NOT NULL, b INTEGER NOT NULL );", db);
    Table & table = *db.getTable("t");
    QueryContext ctx(db);
    ctx.executionContext.branchId = master_branch_id;
    db.constructBranchLineage(master_branch_id, ctx.executionContext);
    for (uint64_t i = 0; i < n; ++i) {
        auto tuple = makeTuple(table, i, b);
        insert_tuple(tuple, table, ctx);
    }
    return table;
}

static void updateRandom(Database & db, Table & table, branch_id_t branch, uint64_t n, double frac, std::mt19937_64 & rng, uint64_t value) {
    QueryContext ctx(db);
    ctx.executionContext.branchId = branch;
    db.constructBranchLineage(branch, ctx.executionContext);
    std::uniform_int_distribution<uint64_t> dist(0, n - 1);
    const uint64_t count = static_cast<uint64_t>(frac * n);
    for (uint64_t i = 0; i < count; ++i) {
        tid_t tid = dist(rng);
        auto tuple = makeTuple(table, tid, value);
        update_tuple(tid, tuple, table, ctx);
    }
}

// scan with a predicate that matches no tuple: bitmap test, chain walk and
// attribute loads are executed, but nothing is materialised for the client
static double scanMicros(Database & db, const std::string & branch) {
    const std::string query = "SELECT a FROM t VERSION " + branch + " WHERE a = 2147483000;";
    std::vector<double> times;
    QueryCompiler::compileAndBenchmark(query, db, (void *) &noop);   // warm-up, discarded
    for (uint64_t r = 0; r < FLAGS_runs; ++r) {
        auto result = QueryCompiler::compileAndBenchmark(query, db, (void *) &noop);
        times.push_back(result.executionTime);
    }
    std::sort(times.begin(), times.end());
    return times[times.size() / 2];
}

using Clock = std::chrono::high_resolution_clock;
static double msSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

static void depthMode() {
    std::cout << "depth,n,frac,scan_leaf_ms,scan_main_ms,leaf_mtuples_per_s,main_mtuples_per_s,rss_mib,identifiers,relabels,version_map_ms" << std::endl;
    for (uint64_t depth : parseList(FLAGS_depths)) {
        std::mt19937_64 rng(FLAGS_seed);
        Database db;
        Table & table = load(db, FLAGS_n);
        std::string parent = "master";
        for (uint64_t i = 1; i <= depth; ++i) {
            std::string name = "b" + std::to_string(i);
            QueryCompiler::compileAndExecute("CREATE BRANCH " + name + " FROM " + parent + ";", db);
            branch_id_t id = db._branchMapping[name];
            updateRandom(db, table, id, FLAGS_n, FLAGS_frac, rng, i);
            updateRandom(db, table, master_branch_id, FLAGS_n, FLAGS_frac, rng, 1000000 + i);
            parent = name;
        }
        double versionMapMs = 0;
        if (FLAGS_version_map > 0 && depth > 0) {
            // the leaf reads its recorded versions instead of walking the chains
            auto start = Clock::now();
            build_version_map(table, db._branchMapping[parent]);
            versionMapMs = msSince(start);
        }
        double leaf = scanMicros(db, parent);
        double main = scanMicros(db, "master");
        std::cout << depth << "," << FLAGS_n << "," << FLAGS_frac << ","
                  << leaf / 1000.0 << "," << main / 1000.0 << ","
                  << FLAGS_n / leaf << "," << FLAGS_n / main << ","
                  << rssMiB() << "," << db.getIdentCount() << "," << db.getRelabelCount() << "," << versionMapMs << std::endl;
    }
}

// agentic mode --------------------------------------------------------------

struct Phases {
    double fork = 0, mutate = 0, evaluate = 0;   // milliseconds
    uint64_t forks = 0, evaluations = 0;
};


static void forkBranch(Database & db, const std::string & name, const std::string & parent, Phases & ph) {
    auto start = Clock::now();
    QueryCompiler::compileAndExecute("CREATE BRANCH " + name + " FROM " + parent + ";", db);
    ph.fork += msSince(start);
    ++ph.forks;
}

static void mutate(Database & db, Table & table, const std::string & name, std::mt19937_64 & rng, Phases & ph) {
    auto start = Clock::now();
    branch_id_t branch = db._branchMapping[name];
    updateRandom(db, table, branch, FLAGS_n, double(FLAGS_mutations) / FLAGS_n, rng, rng() % 1000);
    if (FLAGS_churn > 0) {
        // deletes change the membership of tuples the branch shares; inserts add tuples of its own
        QueryContext ctx(db);
        ctx.executionContext.branchId = branch;
        db.constructBranchLineage(branch, ctx.executionContext);
        for (uint64_t i = 0; i < FLAGS_churn; ++i) {
            delete_tuple(rng() % FLAGS_n, table, ctx);
            auto tuple = makeTuple(table, table._version_mgmt_column.size(), rng() % 1000);
            insert_tuple(tuple, table, ctx);
        }
    }
    ph.mutate += msSince(start);
}

/// \returns The memory of the membership structures of all tables in MiB
static double membershipMiB(Database & db) {
    size_t bytes = 0;
    for (auto & [name, table] : db.getTables()) bytes += table->getMembership().bytes();
    return bytes / (1024.0 * 1024.0);
}

// the agent's objective: a full scan of the branch (a point predicate, so the scan dominates);
// the score is a deterministic function of the branch so that the search is reproducible
static int32_t evalTarget;
static int32_t evalResult;
static void evalCallback(Native::Sql::SqlTuple * tuple) {
    evalResult = static_cast<Native::Sql::Integer &>(*tuple->values[0]).value;
}

static double evaluate(Database & db, const std::string & name, std::mt19937_64 & rng, Phases & ph) {
    auto start = Clock::now();
    evalResult = -1;
    QueryCompiler::compileAndExecute("SELECT b FROM t VERSION " + name + " WHERE a = " + std::to_string(evalTarget) + ";", db, (void *) &evalCallback);
    ph.evaluate += msSince(start);
    ++ph.evaluations;
    return std::uniform_real_distribution<double>(0, 1)(rng);
}

static void agenticMode() {
    std::mt19937_64 rng(FLAGS_seed);
    Database db;
    Table & table = load(db, FLAGS_n);
    evalTarget = static_cast<int32_t>(FLAGS_n / 2);
    Phases ph;
    uint64_t maxDepth = 0;
    uint64_t counter = 0;
    auto newName = [&]() { return "x" + std::to_string(++counter); };
    auto total = Clock::now();

    if (FLAGS_topology == "mcts") {
        // selection by UCB1, expansion by a fork, simulation by mutate + evaluate, backpropagation
        struct Node { std::string name; int64_t parent; uint64_t depth; double visits = 0, value = 0; std::vector<size_t> children; };
        std::vector<Node> tree = {{"master", -1, 0}};
        for (uint64_t it = 0; it < FLAGS_iterations; ++it) {
            size_t node = 0;
            while (tree[node].children.size() >= FLAGS_width) {
                size_t best = tree[node].children[0];
                double bestUcb = -1;
                for (size_t c : tree[node].children) {
                    double ucb = tree[c].value / tree[c].visits + std::sqrt(2.0 * std::log(tree[node].visits) / tree[c].visits);
                    if (ucb > bestUcb) { bestUcb = ucb; best = c; }
                }
                node = best;
            }
            Node child{newName(), int64_t(node), tree[node].depth + 1};
            forkBranch(db, child.name, tree[node].name, ph);
            mutate(db, table, child.name, rng, ph);
            double score = evaluate(db, child.name, rng, ph);
            maxDepth = std::max(maxDepth, child.depth);
            tree.push_back(child);
            size_t id = tree.size() - 1;
            tree[node].children.push_back(id);
            for (int64_t n = id; n >= 0; n = tree[n].parent) { tree[n].visits += 1; tree[n].value += score; }
        }
    } else if (FLAGS_topology == "beam") {
        // each round forks width candidates from the current best and keeps the best one
        std::string base = "master";
        for (uint64_t round = 0; round < FLAGS_iterations; ++round) {
            std::string best;
            double bestScore = -1;
            for (uint64_t w = 0; w < FLAGS_width; ++w) {
                std::string name = newName();
                forkBranch(db, name, base, ph);
                mutate(db, table, name, rng, ph);
                double score = evaluate(db, name, rng, ph);
                if (score > bestScore) { bestScore = score; best = name; }
            }
            base = best;
            maxDepth = round + 1;
        }
    } else if (FLAGS_topology == "bisect") {
        // main advances through a history of snapshots; the agent bisects it for the first
        // failing snapshot, forking a scratch branch from each probe to run the test in
        std::vector<std::string> snapshots;
        for (uint64_t i = 0; i < FLAGS_iterations; ++i) {
            std::string snap = "s" + std::to_string(i);
            forkBranch(db, snap, "master", ph);
            snapshots.push_back(snap);
            mutate(db, table, "master", rng, ph);
        }
        const size_t failing = rng() % snapshots.size();
        size_t lo = 0, hi = snapshots.size() - 1;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            std::string probe = newName();
            forkBranch(db, probe, snapshots[mid], ph);
            mutate(db, table, probe, rng, ph);        // the test writes its own fixtures
            evaluate(db, probe, rng, ph);
            if (mid >= failing) hi = mid; else lo = mid + 1;
        }
        maxDepth = 2;
    } else {
        throw std::runtime_error("unknown topology");
    }

    double totalMs = msSince(total);
    std::cout << "topology,n,iterations,width,mutations,branches,max_depth,fork_ms,mutate_ms,evaluate_ms,total_ms,eval_ms_avg,rss_mib,identifiers,relabels,churn,membership_mib" << std::endl;
    std::cout << FLAGS_topology << "," << FLAGS_n << "," << FLAGS_iterations << "," << FLAGS_width << "," << FLAGS_mutations << ","
              << db._branches.size() << "," << maxDepth << "," << ph.fork << "," << ph.mutate << "," << ph.evaluate << ","
              << totalMs << "," << ph.evaluate / std::max<uint64_t>(ph.evaluations, 1) << "," << rssMiB() << ","
              << db.getIdentCount() << "," << db.getRelabelCount() << "," << FLAGS_churn << "," << membershipMiB(db) << std::endl;
}

// crossbranch mode ----------------------------------------------------------

static std::map<std::string, int64_t> branchSums;
static void collectBranchSum(Native::Sql::SqlTuple * tuple) {
    auto name = static_cast<Native::Sql::Text &>(*tuple->values[0]).getString();
    branchSums[std::string(reinterpret_cast<const char *>(name.second), name.first)] = static_cast<Native::Sql::Numeric &>(*tuple->values[1]).value;
}
static std::string singleBranch;
static void collectSingleSum(Native::Sql::SqlTuple * tuple) {
    branchSums[singleBranch] = static_cast<Native::Sql::Numeric &>(*tuple->values[0]).value;
}

static double queryMicros(Database & db, const std::string & query, void * callback, uint64_t runs) {
    std::vector<double> times;
    QueryCompiler::compileAndBenchmark(query, db, callback);   // warm-up, discarded
    for (uint64_t r = 0; r < runs; ++r) {
        times.push_back(QueryCompiler::compileAndBenchmark(query, db, callback).executionTime);
    }
    std::sort(times.begin(), times.end());
    return times[times.size() / 2];
}

static void crossbranchMode() {
    using Strategy = Algebra::Logical::BranchAggregation::Strategy;
    std::cout << "k,n,frac,mainfrac,aggregates,threads,divergent_frac,separate_ms,perbranch_ms,shared_ms,rss_mib" << std::endl;
    for (uint64_t k : parseList(FLAGS_ks)) {
        std::mt19937_64 rng(FLAGS_seed);
        Database db;
        Table & table = load(db, FLAGS_n, 1);
        std::vector<bool> touched(FLAGS_n, false);
        for (uint64_t i = 1; i <= k; ++i) {
            const std::string name = "c" + std::to_string(i);
            QueryCompiler::compileAndExecute("CREATE BRANCH " + name + " FROM master;", db);
            QueryContext ctx(db);
            ctx.executionContext.branchId = db._branchMapping[name];
            db.constructBranchLineage(ctx.executionContext.branchId, ctx.executionContext);
            std::uniform_int_distribution<uint64_t> dist(0, FLAGS_n - 1);
            for (uint64_t u = 0; u < static_cast<uint64_t>(FLAGS_frac * FLAGS_n); ++u) {
                tid_t tid = dist(rng);
                auto tuple = makeTuple(table, tid, i);
                update_tuple(tid, tuple, table, ctx);
                touched[tid] = true;
            }
        }
        if (FLAGS_mainfrac > 0) {
            updateRandom(db, table, master_branch_id, FLAGS_n, FLAGS_mainfrac, rng, 0);
        }
        const double divergent = std::count(touched.begin(), touched.end(), true) / static_cast<double>(FLAGS_n);

        const std::string grouped = "SELECT branch, " + FLAGS_aggregates + " FROM t VERSION ALL DESCENDANTS OF master GROUP BY branch;";
        // k queries, one per branch, as an agent issues them today, and one query with k scans
        const bool baselines = (k <= FLAGS_maxk_baseline);
        double separate = NAN, perBranch = NAN;
        std::map<std::string, int64_t> expected;
        Algebra::Logical::BranchAggregation::_strategy = Strategy::PerBranch;
        if (baselines) {
            separate = 0;
            for (uint64_t i = 1; i <= k; ++i) {
                singleBranch = "c" + std::to_string(i);
                separate += queryMicros(db, "SELECT " + FLAGS_aggregates + " FROM t VERSION " + singleBranch + ";", (void *) &collectSingleSum, std::min<uint64_t>(FLAGS_runs, 3));
            }
            expected.swap(branchSums);
            branchSums.clear();
            perBranch = queryMicros(db, grouped, (void *) &collectBranchSum, FLAGS_runs);
            if (branchSums != expected) {
                std::cerr << "MISMATCH at k=" << k << " between separate queries and one query with k scans" << std::endl;
                std::exit(1);
            }
        } else {
            // a single run as the reference for the shared scan
            QueryCompiler::compileAndExecute(grouped, db, (void *) &collectBranchSum);
            expected.swap(branchSums);
        }
        // one query, branch-shared scan
        Algebra::Logical::BranchAggregation::_strategy = Strategy::Shared;
        branchSums.clear();
        const double shared = queryMicros(db, grouped, (void *) &collectBranchSum, FLAGS_runs);
        if (branchSums != expected) {
            std::cerr << "MISMATCH at k=" << k << " for the shared scan" << std::endl;
            std::exit(1);
        }

        std::cout << k << "," << FLAGS_n << "," << FLAGS_frac << "," << FLAGS_mainfrac << ",\"" << FLAGS_aggregates << "\","
                  << Parallel::threads << "," << divergent << "," << separate / 1000.0 << "," << perBranch / 1000.0 << "," << shared / 1000.0 << "," << rssMiB() << std::endl;
    }
}

// verify mode ---------------------------------------------------------------

static std::map<int32_t, int32_t> scanned;

// the projection does not preserve the order of the attributes, so b is told apart by its range
static constexpr int32_t bOffset = 10000000;

static void collect(Native::Sql::SqlTuple * tuple) {
    auto a = static_cast<Native::Sql::Integer &>(*tuple->values[0]).value;
    auto b = static_cast<Native::Sql::Integer &>(*tuple->values[1]).value;
    if (a >= bOffset) std::swap(a, b);
    if (!scanned.emplace(a, b).second) throw std::runtime_error("tuple " + std::to_string(a) + " returned twice");
}

struct RefBranch {
    std::string name;
    std::vector<bool> present;  // indexed by tid
    std::vector<int32_t> b;
    int parent = -1;
};

struct Aggregates {
    int64_t count = 0, sum = 0;
    int32_t min = 0, max = 0, avg = 0;
    bool operator==(const Aggregates & o) const { return count == o.count && sum == o.sum && min == o.min && max == o.max && avg == o.avg; }
    bool operator!=(const Aggregates & o) const { return !(*this == o); }
};

static std::map<std::string, Aggregates> aggregated;

// SELECT branch, count(*), sum(b), min(b), max(b), avg(b) ... GROUP BY branch
static void collectAggregates(Native::Sql::SqlTuple * tuple) {
    auto name = static_cast<Native::Sql::Text &>(*tuple->values[0]).getString();
    Aggregates a;
    a.count = static_cast<Native::Sql::Numeric &>(*tuple->values[1]).value;
    a.sum = static_cast<Native::Sql::Numeric &>(*tuple->values[2]).value;
    a.min = static_cast<Native::Sql::Integer &>(*tuple->values[3]).value;
    a.max = static_cast<Native::Sql::Integer &>(*tuple->values[4]).value;
    a.avg = static_cast<Native::Sql::Integer &>(*tuple->values[5]).value;
    if (!aggregated.emplace(std::string(reinterpret_cast<const char *>(name.second), name.first), a).second) {
        throw std::runtime_error("branch returned twice");
    }
}

// SELECT count(*), sum(b) FROM t VERSION x
static void collectSingle(Native::Sql::SqlTuple * tuple) {
    Aggregates a;
    a.count = static_cast<Native::Sql::Numeric &>(*tuple->values[0]).value;
    a.sum = static_cast<Native::Sql::Numeric &>(*tuple->values[1]).value;
    if (!aggregated.emplace("", a).second) throw std::runtime_error("more than one group");
}

static std::map<std::string, Aggregates> expectAggregates(const std::vector<RefBranch> & ref, const std::vector<size_t> & branches, int64_t onlyA) {
    std::map<std::string, Aggregates> expected;
    for (size_t bi : branches) {
        Aggregates a;
        a.min = std::numeric_limits<int32_t>::max();
        a.max = std::numeric_limits<int32_t>::min();
        for (size_t tid = 0; tid < ref[bi].present.size(); ++tid) {
            if (!ref[bi].present[tid] || (onlyA >= 0 && static_cast<int64_t>(tid) != onlyA)) continue;
            int32_t b = ref[bi].b[tid];
            ++a.count;
            a.sum += b;
            a.min = std::min(a.min, b);
            a.max = std::max(a.max, b);
        }
        if (a.count == 0) continue; // no group
        a.avg = static_cast<int32_t>(a.sum / a.count);
        expected[ref[bi].name] = a;
    }
    return expected;
}

static bool verifyCrossBranch(Database & db, std::vector<RefBranch> & ref, std::mt19937_64 & rng) {
    using Strategy = Algebra::Logical::BranchAggregation::Strategy;
    struct Case { std::string version, where; std::vector<size_t> branches; int64_t onlyA; };
    std::vector<Case> cases;

    std::vector<size_t> all(ref.size());
    std::iota(all.begin(), all.end(), 0);
    cases.push_back({"ALL", "", all, -1});

    // a random set, with and without a predicate
    std::vector<size_t> subset;
    std::string names;
    for (size_t bi : all) {
        if (rng() % 3 != 0 && bi != 0) continue;
        subset.push_back(bi);
        names += (names.empty() ? "" : " , ") + ref[bi].name;
    }
    cases.push_back({"( " + names + " )", "", subset, -1});
    const int64_t a = rng() % ref[0].present.size();
    cases.push_back({"( " + names + " )", " WHERE a = " + std::to_string(a), subset, a});

    // descendants and leaves of a random branch
    const size_t root = rng() % ref.size();
    std::vector<size_t> descendants, leaves;
    for (size_t bi : all) {
        bool below = false;
        for (int p = ref[bi].parent; p >= 0; p = ref[p].parent) if (static_cast<size_t>(p) == root) { below = true; break; }
        if (!below) continue;
        descendants.push_back(bi);
        bool hasChild = false;
        for (auto & r : ref) if (r.parent == static_cast<int>(bi)) hasChild = true;
        if (!hasChild) leaves.push_back(bi);
    }
    if (!descendants.empty()) {
        cases.push_back({"ALL DESCENDANTS OF " + ref[root].name, "", descendants, -1});
        cases.push_back({"ALL LEAVES OF " + ref[root].name, "", leaves, -1});
    }

    bool ok = true;
    for (Strategy strategy : {Strategy::Shared, Strategy::PerBranch}) {
        Algebra::Logical::BranchAggregation::_strategy = strategy;
        const char * label = (strategy == Strategy::Shared) ? "shared" : "per branch";
        for (auto & c : cases) {
            aggregated.clear();
            QueryCompiler::compileAndExecute("SELECT branch, count(*), sum(b), min(b), max(b), avg(b) FROM t VERSION " + c.version +
                                             c.where + " GROUP BY branch;", db, (void *) &collectAggregates);
            auto expected = expectAggregates(ref, c.branches, c.onlyA);
            if (aggregated != expected) {
                size_t wrong = 0;
                for (auto & [name, e] : expected) if (!aggregated.count(name) || aggregated[name] != e) ++wrong;
                std::cout << "MISMATCH (" << label << ") in VERSION " << c.version << c.where << ": " << wrong << " of " << expected.size()
                          << " groups wrong, " << aggregated.size() << " returned" << std::endl;
                ok = false;
            }
        }
        // one branch without GROUP BY
        const size_t bi = rng() % ref.size();
        aggregated.clear();
        QueryCompiler::compileAndExecute("SELECT count(*), sum(b) FROM t VERSION " + ref[bi].name + ";", db, (void *) &collectSingle);
        auto expected = expectAggregates(ref, {bi}, -1);
        if (!expected.empty() && (aggregated.size() != 1 || aggregated[""].count != expected.begin()->second.count || aggregated[""].sum != expected.begin()->second.sum)) {
            std::cout << "MISMATCH (" << label << ") in single branch " << ref[bi].name << std::endl;
            ok = false;
        }
    }
    Algebra::Logical::BranchAggregation::_strategy = Strategy::Shared;
    return ok;
}

static bool verifyAll(Database & db, std::vector<RefBranch> & ref) {
    bool ok = true;
    for (auto & branch : ref) {
        scanned.clear();
        QueryCompiler::compileAndExecute("SELECT a, b FROM t VERSION " + branch.name + ";", db, (void *) &collect);
        std::map<int32_t, int32_t> expected;
        for (size_t tid = 0; tid < branch.present.size(); ++tid) {
            if (branch.present[tid]) expected[static_cast<int32_t>(tid)] = branch.b[tid];
        }
        if (scanned != expected) {
            size_t missing = 0, extra = 0, wrong = 0;
            for (auto & [a, b] : expected) {
                auto it = scanned.find(a);
                if (it == scanned.end()) ++missing; else if (it->second != b) ++wrong;
            }
            for (auto & [a, b] : scanned) if (!expected.count(a)) ++extra;
            std::cout << "MISMATCH in " << branch.name << ": missing " << missing << ", extra " << extra << ", wrong value " << wrong << std::endl;
            ok = false;
        }
    }
    if (FLAGS_crossbranch) {
        std::mt19937_64 rng(ref.size() * 7919 + ref[0].present.size());
        ok = verifyCrossBranch(db, ref, rng) && ok;
    }
    return ok;
}

static void verifyMode() {
    std::mt19937_64 rng(FLAGS_seed);
    Database db;
    Table & table = load(db, FLAGS_n, bOffset);
    std::vector<RefBranch> ref(1);
    ref[0].name = "master";
    ref[0].present.assign(FLAGS_n, true);
    ref[0].b.assign(FLAGS_n, bOffset);
    uint64_t forks = 0, updates = 0, inserts = 0, deletes = 0;
    bool ok = true;

    for (uint64_t op = 1; op <= FLAGS_ops && ok; ++op) {
        const uint64_t kind = rng() % 100;
        size_t bi = rng() % ref.size();
        branch_id_t branch = db._branchMapping[ref[bi].name];
        QueryContext ctx(db);
        ctx.executionContext.branchId = branch;
        db.constructBranchLineage(branch, ctx.executionContext);
        if (kind < 10) {
            if (FLAGS_deep) {
                bi = ref.size() - 1;
                branch = db._branchMapping[ref[bi].name];
            }
            // fork a random branch
            RefBranch child = ref[bi];
            child.name = "v" + std::to_string(ref.size());
            child.parent = static_cast<int>(bi);
            QueryCompiler::compileAndExecute("CREATE BRANCH " + child.name + " FROM " + ref[bi].name + ";", db);
            ref.push_back(std::move(child));
            ++forks;
        } else if (kind < 80 || kind >= 90) {
            // update or delete a random tuple of the branch
            std::vector<size_t> tids;
            for (int attempt = 0; attempt < 32 && tids.empty(); ++attempt) {
                size_t tid = rng() % ref[bi].present.size();
                if (ref[bi].present[tid]) tids.push_back(tid);
            }
            if (tids.empty()) continue;
            size_t tid = tids[0];
            if (kind < 80) {
                int32_t value = bOffset + static_cast<int32_t>(rng() % 1000000);
                auto tuple = makeTuple(table, tid, value);
                update_tuple(tid, tuple, table, ctx);
                ref[bi].b[tid] = value;
                ++updates;
            } else {
                delete_tuple(tid, table, ctx);
                ref[bi].present[tid] = false;
                ++deletes;
            }
        } else {
            // insert into the branch; the tuple exists only there and in its later forks
            int32_t value = bOffset + static_cast<int32_t>(rng() % 1000000);
            size_t tid = table._version_mgmt_column.size();
            auto tuple = makeTuple(table, tid, value);
            tid_t got = insert_tuple(tuple, table, ctx);
            if (got != tid) throw std::runtime_error("unexpected tid");
            for (auto & r : ref) { r.present.push_back(false); r.b.push_back(0); }
            ref[bi].present[tid] = true;
            ref[bi].b[tid] = value;
            ++inserts;
        }
        if (op % 500 == 0) ok = verifyAll(db, ref);
    }
    if (ok) ok = verifyAll(db, ref);
    std::cout << (ok ? "OK" : "FAILED") << ": branches " << ref.size() << ", forks " << forks << ", updates " << updates
              << ", inserts " << inserts << ", deletes " << deletes << ", identifiers " << db.getIdentCount()
              << ", relabels " << db.getRelabelCount() << std::endl;
    if (!ok) std::exit(1);
}

// cycles mode --------------------------------------------------------------

// R3: create-and-drop cycles: fork a branch from main, the branch and main each update frac of the
// tuples, scan the branch, drop it; memory must not grow with the number of cycles
static void cyclesMode() {
    std::mt19937_64 rng(FLAGS_seed);
    Database db;
    Table & table = load(db, FLAGS_n);
    if (FLAGS_keep) QueryCompiler::compileAndExecute("CREATE BRANCH keep FROM master;", db);
    std::cout << "cycle,n,frac,gc,keep,versions,rss_mib,membership_mib,drop_ms,cycle_ms,scan_main_ms" << std::endl;
    double dropMs = 0, cycleMs = 0;
    uint64_t since = 0;
    for (uint64_t cycle = 1; cycle <= FLAGS_iterations; ++cycle) {
        auto start = Clock::now();
        QueryCompiler::compileAndExecute("CREATE BRANCH c FROM master;", db);
        updateRandom(db, table, db._branchMapping["c"], FLAGS_n, FLAGS_frac, rng, rng() % 1000);
        updateRandom(db, table, master_branch_id, FLAGS_n, FLAGS_frac, rng, rng() % 1000);
        scanMicros(db, "c");
        auto drop = Clock::now();
        QueryCompiler::compileAndExecute("DROP BRANCH c;", db);
        dropMs += msSince(drop);
        cycleMs += msSince(start);
        ++since;
        if (cycle % 20 == 0 || cycle == FLAGS_iterations) {
            std::cout << cycle << "," << FLAGS_n << "," << FLAGS_frac << "," << FLAGS_gc << "," << FLAGS_keep << ","
                      << live_versions() << "," << rssMiB() << "," << membershipMiB(db) << "," << dropMs / since << ","
                      << cycleMs / since << "," << scanMicros(db, "master") / 1000.0 << std::endl;
            dropMs = cycleMs = 0;
            since = 0;
        }
    }
}

// diffmerge mode -----------------------------------------------------------

static size_t diffRows;
static void countDiffRow(Native::Sql::SqlTuple *) { ++diffRows; }

// DIFF and MERGE of a branch forked from main after the branch and main each updated d tuples (to
// the same value, so that the merge has no conflicts); over the diverging tuples and over the table
static void diffmergeMode() {
    std::cout << "divergence,n,threads,diff_rows,diff_ms,diff_scan_ms,merge_ms,merge_scan_ms" << std::endl;
    for (uint64_t d : parseList(FLAGS_divergences)) {
        std::vector<double> times[2][2]; // [scan][diff, merge]
        size_t rows[2] = {0, 0};
        for (uint64_t r = 0; r < FLAGS_runs; ++r) {
            for (int scan = 0; scan < 2; ++scan) {
                std::mt19937_64 rng(FLAGS_seed + r);
                Database db;
                Table & table = load(db, FLAGS_n);
                QueryCompiler::compileAndExecute("CREATE BRANCH b FROM master;", db);
                updateRandom(db, table, db._branchMapping["b"], FLAGS_n, double(d) / FLAGS_n, rng, 7);
                updateRandom(db, table, master_branch_id, FLAGS_n, double(d) / FLAGS_n, rng, 7);
                diff_scans_whole_table = (scan == 1);
                diffRows = 0;
                auto start = Clock::now();
                QueryCompiler::compileAndExecute("SELECT * FROM DIFF(t VERSION b, t VERSION master);", db, (void *) &countDiffRow);
                times[scan][0].push_back(msSince(start));
                rows[scan] = diffRows;
                start = Clock::now();
                QueryCompiler::compileAndExecute("MERGE BRANCH b INTO master;", db);
                times[scan][1].push_back(msSince(start));
                diff_scans_whole_table = false;
            }
        }
        if (rows[0] != rows[1]) throw std::runtime_error("DIFF over the diverging tuples and over the table differ");
        auto median = [](std::vector<double> v) { std::sort(v.begin(), v.end()); return v[v.size() / 2]; };
        std::cout << d << "," << FLAGS_n << "," << Parallel::threads << "," << rows[0] << "," << median(times[0][0]) << ","
                  << median(times[1][0]) << "," << median(times[0][1]) << "," << median(times[1][1]) << std::endl;
    }
}

// concurrent mode ----------------------------------------------------------

// t ( a, b, g ): the n / mutations tuples with the same g form the target of an update
static void loadGrouped(Database & db, uint64_t n, uint64_t groups) {
    QueryCompiler::compileAndExecute("CREATE TABLE t ( a INTEGER NOT NULL, b INTEGER NOT NULL, g INTEGER NOT NULL );", db);
    Table & table = *db.getTable("t");
    QueryContext ctx(db);
    ctx.executionContext.branchId = master_branch_id;
    db.constructBranchLineage(master_branch_id, ctx.executionContext);
    for (uint64_t i = 0; i < n; ++i) {
        std::vector<std::unique_ptr<Native::Sql::Value>> values;
        values.push_back(Native::Sql::Value::castString(std::to_string(i), table.getCI("a")->type));
        values.push_back(Native::Sql::Value::castString("0", table.getCI("b")->type));
        values.push_back(Native::Sql::Value::castString(std::to_string(i % groups), table.getCI("g")->type));
        Native::Sql::SqlTuple tuple(std::move(values));
        insert_tuple(tuple, table, ctx);
    }
}

// per statement: the time until it returned and the time its compiled code ran
struct Latencies {
    std::vector<double> eval, mutate, evalExec, mutateExec;
    void add(const Latencies & other) {
        eval.insert(eval.end(), other.eval.begin(), other.eval.end());
        mutate.insert(mutate.end(), other.mutate.begin(), other.mutate.end());
        evalExec.insert(evalExec.end(), other.evalExec.begin(), other.evalExec.end());
        mutateExec.insert(mutateExec.end(), other.mutateExec.begin(), other.mutateExec.end());
    }
};

static double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<size_t>(p * v.size()))];
}

/// Runs the clients for FLAGS_seconds; client i works in branch c<i>. Roles: 'a' agent (update, then
/// scan), 'r' reader (scans), 'w' writer (updates), 'm' writer of main, 'i' inserter. \returns The latencies per client
static std::vector<Latencies> runClients(Database & db, const std::string & roles, uint64_t groups) {
    std::atomic<bool> stop(false);
    std::vector<Latencies> latencies(roles.size());
    std::vector<std::thread> threads;
    for (size_t c = 0; c < roles.size(); ++c) {
        threads.emplace_back([&, c] {
            const std::string branch = (roles[c] == 'm') ? "main" : "c" + std::to_string(c);
            std::mt19937_64 rng(FLAGS_seed + c);
            for (uint64_t inserted = 0; !stop; ) {
                if (roles[c] == 'i') {
                    // keys above those of the table, distinct per client
                    const uint64_t key = FLAGS_n * (c + 1) + inserted++;
                    auto start = Clock::now();
                    auto result = QueryCompiler::compileAndBenchmark("INSERT INTO t VERSION " + branch + " ( a , b , g ) VALUES ( " + std::to_string(key) + " , 0 , " + std::to_string(key % groups) + " );", db, (void *) &noop);
                    latencies[c].mutate.push_back(msSince(start));
                    latencies[c].mutateExec.push_back(result.executionTime / 1000.0);
                    continue;
                }
                if (roles[c] != 'r') {
                    auto start = Clock::now();
                    auto result = QueryCompiler::compileAndBenchmark("UPDATE t VERSION " + branch + " SET b = b + 1 WHERE g = " + std::to_string(rng() % groups) + ";", db, (void *) &noop);
                    latencies[c].mutate.push_back(msSince(start));
                    latencies[c].mutateExec.push_back(result.executionTime / 1000.0);
                }
                if (roles[c] != 'w' && roles[c] != 'm') {
                    auto start = Clock::now();
                    auto result = QueryCompiler::compileAndBenchmark("SELECT b FROM t VERSION " + branch + " WHERE a = " + std::to_string(rng() % FLAGS_n) + ";", db, (void *) &noop);
                    latencies[c].eval.push_back(msSince(start));
                    latencies[c].evalExec.push_back(result.executionTime / 1000.0);
                }
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::duration<double>(FLAGS_seconds));
    stop = true;
    for (auto & thread : threads) thread.join();
    return latencies;
}

static void concurrentMode() {
    if (FLAGS_latching == "serial") QueryCompiler::latching = QueryCompiler::Latching::Serial;
    else if (FLAGS_latching == "table") QueryCompiler::latching = QueryCompiler::Latching::Table;
    else if (FLAGS_latching == "branch") QueryCompiler::latching = QueryCompiler::Latching::Branch;
    else throw std::runtime_error("unknown latching " + FLAGS_latching);
    const uint64_t groups = std::max<uint64_t>(1, FLAGS_n / FLAGS_mutations);
    std::cout << "scenario,latching,clients,threads,n,iterations_per_s,eval_ms_p50,eval_ms_p99,mutate_ms_p50,mutate_ms_p99,"
                 "eval_exec_ms_p50,mutate_exec_ms_p50,sibling_per_s" << std::endl;
    auto run = [&](const std::string & scenario, const std::string & roles) {
        Database db;
        loadGrouped(db, FLAGS_n, groups);
        for (size_t c = 0; c < roles.size(); ++c) {
            QueryCompiler::compileAndExecute("CREATE BRANCH c" + std::to_string(c) + " FROM master;", db);
        }
        auto latencies = runClients(db, roles, groups);
        // agents: all clients; isolation: client 0 is measured, client 1 is the sibling
        Latencies measured;
        size_t iterations = 0;
        for (size_t c = 0; c < roles.size(); ++c) {
            if (roles[c] == 'a' || c == 0) {
                measured.add(latencies[c]);
                iterations += std::max(latencies[c].eval.size(), latencies[c].mutate.size());
            }
        }
        // the sibling, or the client beside agents
        const bool besideAgents = (roles[0] == 'a' && roles.back() != 'a');
        const size_t other = besideAgents ? roles.size() - 1 : 1;
        const double sibling = (roles.size() > 1 && (roles[0] != 'a' || besideAgents))
                ? std::max(latencies[other].eval.size(), latencies[other].mutate.size()) / FLAGS_seconds : 0;
        std::cout << scenario << "," << FLAGS_latching << "," << roles.size() << "," << Parallel::threads << ","
                  << FLAGS_n << "," << iterations / FLAGS_seconds << "," << percentile(measured.eval, 0.5) << ","
                  << percentile(measured.eval, 0.99) << "," << percentile(measured.mutate, 0.5) << ","
                  << percentile(measured.mutate, 0.99) << "," << percentile(measured.evalExec, 0.5) << ","
                  << percentile(measured.mutateExec, 0.5) << "," << sibling << std::endl;
    };
    for (uint64_t k : parseList(FLAGS_clients)) run("agents", std::string(k, 'a'));
    run("reader_alone", "r");
    run("reader_sibling_reads", "rr");
    run("reader_sibling_writes", "rw");
    run("reader_main_writes", "rm");
    run("agents_main_writes", std::string(16, 'a') + "m");
    run("reader_sibling_inserts", "ri");
    run("agents_sibling_inserts", std::string(16, 'a') + "i");
}

static void forkMode() {
    std::cout << "n,fork_us_median,rss_mib,membership_mib" << std::endl;
    for (uint64_t n : parseList(FLAGS_sizes)) {
        Database db;
        load(db, n);
        std::vector<double> times;
        for (uint64_t r = 0; r < FLAGS_runs; ++r) {
            auto start = std::chrono::high_resolution_clock::now();
            db.createBranch("f" + std::to_string(r), master_branch_id);
            auto end = std::chrono::high_resolution_clock::now();
            times.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count() / 1000.0);
        }
        std::sort(times.begin(), times.end());
        std::cout << n << "," << times[times.size() / 2] << "," << rssMiB() << "," << membershipMiB(db) << std::endl;
    }
}

int main(int argc, char * argv[]) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    if (FLAGS_threads > 0) Parallel::threads = FLAGS_threads;
    Parallel::morselSize = FLAGS_morsel;
    collect_garbage = overwrite_own_versions = FLAGS_gc;
    version_map_after_scans = FLAGS_version_map;
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    if (FLAGS_mode == "depth") depthMode();
    else if (FLAGS_mode == "fork") forkMode();
    else if (FLAGS_mode == "verify") verifyMode();
    else if (FLAGS_mode == "crossbranch") crossbranchMode();
    else if (FLAGS_mode == "agentic") agenticMode();
    else if (FLAGS_mode == "diffmerge") diffmergeMode();
    else if (FLAGS_mode == "cycles") cyclesMode();
    else if (FLAGS_mode == "concurrent") concurrentMode();
    else { std::cerr << "unknown mode" << std::endl; return 1; }

    llvm::llvm_shutdown();
    return 0;
}
