// Branch microbenchmarks for TardisDB (baseline measurements for the DEFT paper).
//
//   depth: scan throughput in the leaf of a branch chain main -> b1 -> ... -> bD,
//          where every branch and main update a fraction of the tuples after each fork
//   fork:  latency of CREATE BRANCH over the table size
//
// Output is CSV on stdout.

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/TargetSelect.h>

#include "foundations/Database.hpp"
#include "foundations/version_management.hpp"
#include "queryCompiler/queryCompiler.hpp"
#include "sql/SqlValues.hpp"

#include "gflags/gflags.h"

DEFINE_string(mode, "depth", "depth | fork");
DEFINE_uint64(n, 1000000, "tuples");
DEFINE_string(depths, "0,1,2,4,8,16,32,64,128", "branch depths (depth mode)");
DEFINE_string(sizes, "10000,100000,1000000,10000000", "table sizes (fork mode)");
DEFINE_double(frac, 0.01, "fraction of tuples updated per branch and by main after each fork");
DEFINE_uint64(runs, 5, "repetitions per measurement (median reported)");
DEFINE_uint64(seed, 42, "random seed");

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

static Table & load(Database & db, uint64_t n) {
    QueryCompiler::compileAndExecute("CREATE TABLE t ( a INTEGER NOT NULL, b INTEGER NOT NULL );", db);
    Table & table = *db.getTable("t");
    QueryContext ctx(db);
    ctx.executionContext.branchId = master_branch_id;
    db.constructBranchLineage(master_branch_id, ctx.executionContext);
    for (uint64_t i = 0; i < n; ++i) {
        auto tuple = makeTuple(table, i, 0);
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

static void depthMode() {
    std::cout << "depth,n,frac,scan_leaf_ms,scan_main_ms,leaf_mtuples_per_s,main_mtuples_per_s,rss_mib" << std::endl;
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
        double leaf = scanMicros(db, parent);
        double main = scanMicros(db, "master");
        std::cout << depth << "," << FLAGS_n << "," << FLAGS_frac << ","
                  << leaf / 1000.0 << "," << main / 1000.0 << ","
                  << FLAGS_n / leaf << "," << FLAGS_n / main << ","
                  << rssMiB() << std::endl;
    }
}

static void forkMode() {
    std::cout << "n,fork_us_median,rss_mib" << std::endl;
    for (uint64_t n : parseList(FLAGS_sizes)) {
        Database db;
        load(db, n);
        std::vector<double> times;
        // the bitmap holds a fixed number of branch columns, so fork at most runs times
        for (uint64_t r = 0; r < FLAGS_runs; ++r) {
            auto start = std::chrono::high_resolution_clock::now();
            db.createBranch("f" + std::to_string(r), master_branch_id);
            auto end = std::chrono::high_resolution_clock::now();
            times.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count() / 1000.0);
        }
        std::sort(times.begin(), times.end());
        std::cout << n << "," << times[times.size() / 2] << "," << rssMiB() << std::endl;
    }
}

int main(int argc, char * argv[]) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    if (FLAGS_mode == "depth") depthMode();
    else if (FLAGS_mode == "fork") forkMode();
    else { std::cerr << "unknown mode" << std::endl; return 1; }

    llvm::llvm_shutdown();
    return 0;
}
