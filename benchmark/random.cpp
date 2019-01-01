#include "foundations/Database.hpp"
#include "foundations/version_management.hpp"

#include <chrono>
#include <random>
#include <iostream>

#include "native/sql/SqlValues.hpp"
#include "native/sql/SqlTuple.hpp"

static constexpr size_t repetitions = 16;

static constexpr auto master_factor = 0.7;
//static constexpr auto update_factor = 0.2;
static constexpr auto update_factor = 5.0;
static constexpr int seed = 42;
//static constexpr size_t new_tuples_per_episode = 1<<18;
static constexpr size_t new_tuples_per_episode = 1<<19;

static std::mt19937 rd_engine(seed);

using namespace Native::Sql;

void insert_tuples(branch_id_t branch, size_t cnt, Database & db, Table & table) {
    QueryContext ctx(db);
    ctx.executionContext.branchId = branch;
    db.constructBranchLineage(branch, ctx.executionContext);

    std::vector<value_op_t> values;
    values.push_back(std::make_unique<Integer>(1));
    values.push_back(std::make_unique<Integer>(2));
    values.push_back(std::make_unique<Integer>(3));
    SqlTuple tuple(std::move(values));
    for (size_t i = 0; i < cnt; ++i) {
        insert_tuple(tuple, table, ctx);
    }
}

void update_tuples_once(branch_id_t branch, size_t cnt, Database & db, Table & table) {
    QueryContext ctx(db);
    ctx.executionContext.branchId = branch;
    db.constructBranchLineage(branch, ctx.executionContext);

    std::vector<tid_t> tids;
    for (size_t tid = 0; tid < table._version_mgmt_column.size(); ++tid) {
        tids.push_back(tid);
    }
    for (size_t tid = 0; tid < table._dangling_version_mgmt_column.size(); ++tid) {
        tids.push_back(mark_as_dangling_tid(tid));
    }

    std::vector<value_op_t> values;
    values.push_back(std::make_unique<Integer>(1));
    values.push_back(std::make_unique<Integer>(2));
    values.push_back(std::make_unique<Integer>(3));
    SqlTuple tuple(std::move(values));

    std::shuffle(tids.begin(), tids.end(), rd_engine);
    size_t updated = 0;
    for (tid_t tid : tids) {
        VersionEntry * version_entry;
        if (is_marked_as_dangling_tid(tid)) {
            tid_t unmarked = unmark_dangling_tid(tid);
            version_entry = table._dangling_version_mgmt_column[unmarked].get();
        } else {
            version_entry = table._version_mgmt_column[tid].get();
        }
        if (!has_lineage_intersection(ctx, version_entry)) {
            continue;
        }

        update_tuple(tid, tuple, table, ctx);
        updated += 1;
    }
    assert(updated == cnt);
}

void update_tuples(branch_id_t branch, size_t cnt, Database & db, Table & table) {
    QueryContext ctx(db);
    ctx.executionContext.branchId = branch;
    db.constructBranchLineage(branch, ctx.executionContext);

    std::vector<tid_t> tids;
    for (size_t tid = 0; tid < table._version_mgmt_column.size(); ++tid) {
        tids.push_back(tid);
    }
    for (size_t tid = 0; tid < table._dangling_version_mgmt_column.size(); ++tid) {
        tids.push_back(mark_as_dangling_tid(tid));
    }

    std::vector<value_op_t> values;
    values.push_back(std::make_unique<Integer>(1));
    values.push_back(std::make_unique<Integer>(2));
    values.push_back(std::make_unique<Integer>(3));
    SqlTuple tuple(std::move(values));

    std::uniform_int_distribution<size_t> distribution(0, tids.size());
    for (size_t updated = 0; updated < cnt; ++updated) {
        tid_t tid = tids[distribution(rd_engine)];
        if (!is_visible(tid, table, ctx)) {
            continue;
        }
        update_tuple(tid, tuple, table, ctx);
    }
}

std::vector<branch_id_t> get_branches_dist(Database & db) {
    branch_id_t max_branch = db.getLargestBranchId();
    if (max_branch == master_branch_id) {
        return {master_branch_id};
    }
    size_t master_cnt = max_branch/(1.0-master_factor)*master_factor;
    std::vector<branch_id_t> branches_dist;
    for (size_t i = 0; i < master_cnt; ++i) {
        branches_dist.push_back(master_branch_id);
    }
    for (branch_id_t branch = 0; branch <= max_branch; ++branch) {
        if (branch != master_branch_id) {
            branches_dist.push_back(branch);
        }
    }
    std::shuffle(branches_dist.begin(), branches_dist.end(), rd_engine);
    return branches_dist;
}

void perform_bunch_inserts(Database & db, Table & table) {
    auto branches_dist = get_branches_dist(db);
    size_t chunk_size = new_tuples_per_episode/branches_dist.size();
    for (branch_id_t branch : branches_dist) {
        insert_tuples(branch, chunk_size, db, table);
    }
}

void perform_bunch_updates(Database & db, Table & table) {
    auto branches_dist = get_branches_dist(db);
    size_t table_size = table._version_mgmt_column.size() + table._dangling_version_mgmt_column.size();
    size_t total_cnt = table_size*update_factor;
    size_t chunk_size = total_cnt/branches_dist.size();
    for (branch_id_t branch : branches_dist) {
        update_tuples(branch, chunk_size, db, table);
    }
}

inline void print_result(const std::tuple<ScanItem<Register<Integer>>, ScanItem<Register<Integer>>, ScanItem<Register<Integer>>> & scan_items) {
    printf("%d\t%d\t%d\n",
        std::get<0>(scan_items).reg.sql_value.value,
        std::get<1>(scan_items).reg.sql_value.value,
        std::get<2>(scan_items).reg.sql_value.value
    );
}

static size_t tuple_cnt = 0;
static int sum = 0;
inline void sum_consumer(const std::tuple<ScanItem<Register<Integer>>, ScanItem<Register<Integer>>, ScanItem<Register<Integer>>> & scan_items) {
    sum += std::get<0>(scan_items).reg.sql_value.value;
    sum += std::get<1>(scan_items).reg.sql_value.value;
    sum += std::get<2>(scan_items).reg.sql_value.value;
    tuple_cnt += 1;
}

int64_t measure_master_scan_yielding_latest(branch_id_t branch, Database & db, Table & table) {
    QueryContext ctx(db);
    ctx.executionContext.branchId = branch;
    db.constructBranchLineage(branch, ctx.executionContext);

    const auto & column0 = table.getColumn(0);
    const auto & column1 = table.getColumn(1);
    const auto & column2 = table.getColumn(2);

    auto scan_items = std::make_tuple<
        ScanItem<Register<Integer>>, ScanItem<Register<Integer>>, ScanItem<Register<Integer>>>(
        {column0, 0},
        {column1, 4},
        {column2, 8});

    tuple_cnt = 0;

    using namespace std::chrono;
    const auto query_start = high_resolution_clock::now();
    scan_relation_yielding_latest(ctx, table, sum_consumer, scan_items);
    const auto query_duration = high_resolution_clock::now() - query_start;
    auto duration = duration_cast<milliseconds>(query_duration).count();
//    printf("Execution time: %lums\n", duration);
    return duration;
}

int64_t measure_master_scan_yielding_earliest(branch_id_t branch, Database & db, Table & table) {
    QueryContext ctx(db);
    ctx.executionContext.branchId = branch;
    db.constructBranchLineage(branch, ctx.executionContext);

    const auto & column0 = table.getColumn(0);
    const auto & column1 = table.getColumn(1);
    const auto & column2 = table.getColumn(2);

    auto scan_items = std::make_tuple<
        ScanItem<Register<Integer>>, ScanItem<Register<Integer>>, ScanItem<Register<Integer>>>(
        {column0, 0},
        {column1, 4},
        {column2, 8});

    tuple_cnt = 0;

    using namespace std::chrono;
    const auto query_start = high_resolution_clock::now();
    scan_relation_yielding_earliest(ctx, table, sum_consumer, scan_items);
    const auto query_duration = high_resolution_clock::now() - query_start;
    auto duration = duration_cast<milliseconds>(query_duration).count();
//    printf("Execution time: %lums\n", duration);
    return duration;
}

void measure(std::function<int64_t()> bench_func) {
    int64_t acc = 0;
    std::vector<int64_t> samples;
    for (size_t i = 0; i < repetitions; ++i) {
        auto sample = bench_func();
        acc += sample;
        samples.push_back(sample);
    }
    double mean = static_cast<double>(acc)/repetitions;
    double var = 0.0, std;
    for (size_t i = 0; i < repetitions; ++i) {
        var += (samples[i] - mean) * (samples[i] - mean);
    }
    std = std::sqrt(var);
    std::cout << "mean: " << mean << " std: " << std << std::endl;
    std::cout << "tuple count: " << tuple_cnt << std::endl;
    double tuple_cost = mean/tuple_cnt*1.e6;
    std::cout << "per tuple cost: " << tuple_cost << "ns" << std::endl;
}

void run_benchmark() {
    printf("generating data...\n");
    auto db = std::make_unique<Database>();

    auto & bench_table = db->createTable("bench_table");
    bench_table.addColumn("a", Sql::getIntegerTy());
    bench_table.addColumn("b", Sql::getIntegerTy());
    bench_table.addColumn("c", Sql::getIntegerTy());

    std::vector<branch_id_t> branches{ master_branch_id };
    perform_bunch_inserts(*db, bench_table);
    perform_bunch_updates(*db, bench_table);
    branch_id_t branch1 = db->createBranch("branch1", master_branch_id);
    branches.push_back(branch1);
    perform_bunch_inserts(*db, bench_table);
    perform_bunch_updates(*db, bench_table);
    branch_id_t branch2 = db->createBranch("branch2", branch1);
    branches.push_back(branch2);
    perform_bunch_inserts(*db, bench_table);
    perform_bunch_updates(*db, bench_table);
    branch_id_t branch3 = db->createBranch("branch3", invalid_branch_id);
    branches.push_back(branch3);
    perform_bunch_inserts(*db, bench_table);
    perform_bunch_updates(*db, bench_table);
    branch_id_t branch4 = db->createBranch("branch4", branch3);
    branches.push_back(branch4);
    perform_bunch_inserts(*db, bench_table);
    perform_bunch_updates(*db, bench_table);    

    for (branch_id_t branch : branches) {
        printf("measuring scan performance on branch %d yielding earliest tuples...\n", branch);
        measure([&]{
            return measure_master_scan_yielding_earliest(branch, *db, bench_table);
        });
        printf("measuring scan performance on branch %d yielding latest tuples...\n", branch);
        measure([&]{
            return measure_master_scan_yielding_latest(branch, *db, bench_table);
        });
    }
}

int main(int argc, char * argv[]) {
    ModuleGen moduleGen("QueryModule");
    run_benchmark();
}
