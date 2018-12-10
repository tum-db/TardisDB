#pragma once

#include "foundations/Database.hpp"
#include "foundations/version_management.hpp"

#include <random>
#include <iostream>

static constexpr auto master_factor = 0.7;

void insert_data(int seed, branch_id_t branch, Database & db) {
#if 0
    // Seed with a real random value, if available
    std::random_device r;
 
    // Choose a random mean between 1 and 6
    std::default_random_engine e1(r());
    std::uniform_int_distribution<int> uniform_dist(1, 6);
    int mean = uniform_dist(e1);
    std::cout << "Randomly-chosen mean: " << mean << '\n';
 
    // Generate a normal distribution around that mean
    std::seed_seq seed2{r(), r(), r(), r(), r(), r(), r(), r()}; 
    std::mt19937 e2(seed2);
    std::normal_distribution<> normal_dist(mean, 2);
 
    std::map<int, int> hist;
    for (int n = 0; n < 10000; ++n) {
        ++hist[std::round(normal_dist(e2))];
    }
    std::cout << "Normal distribution around " << mean << ":\n";
    for (auto p : hist) {
        std::cout << std::fixed << std::setprecision(1) << std::setw(2)
                  << p.first << ' ' << std::string(p.second/200, '*') << '\n';
    }
#endif
}

void run_benchmark() {
    auto db = std::make_unique<Database>();

    auto bench_table_up = std::make_unique<Table>();
    auto & bench_table = *bench_table_up;
    db->addTable(std::move(bench_table_up), "bench_table");

    bench_table.addColumn("a", Sql::getIntegerTy());
    bench_table.addColumn("b", Sql::getIntegerTy());
    bench_table.addColumn("c", Sql::getIntegerTy());

    QueryContext ctx(*db);
    // set up branch stuff
    branch_id_t branch1 = db->createBranch("branch1", master_branch_id);
    insert_data();
    branch_id_t branch2 = db->createBranch("branch2", branch2);
    // TODO insert
    branch_id_t branch3 = db->createBranch("branch3", invalid_branch_id);
    branch_id_t branch4 = db->createBranch("branch4", branch3);
    // TODO insert

    db->constructBranchLineage(master_branch_id, ctx.executionContext);
}
