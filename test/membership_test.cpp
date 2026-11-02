// Branch membership under forks, deletes and inserts, in particular the entries of lazy
// membership: each branch is compared with a model in which a fork copies its parent

#include "codegen/LLVMCompat.hpp"

#include <random>
#include <set>

#include "foundations/version_management.hpp"
#include "native/sql/SqlTuple.hpp"
#include "queryCompiler/queryCompiler.hpp"
#include <gtest/gtest.h>

namespace {

    std::set<int32_t> scanned;

    void collect(Native::Sql::SqlTuple * tuple) {
        scanned.insert(static_cast<Native::Sql::Integer &>(*tuple->values[0]).value);
    }

    Native::Sql::SqlTuple makeTuple(Table & table, int32_t a) {
        std::vector<std::unique_ptr<Native::Sql::Value>> values;
        values.push_back(Native::Sql::Value::castString(std::to_string(a), table.getCI("a")->type));
        values.push_back(Native::Sql::Value::castString("0", table.getCI("b")->type));
        return Native::Sql::SqlTuple(std::move(values));
    }

    TEST(MembershipTest, AgainstModel) {
        const int32_t n = 4000;
        size_t entries = 0;
        for (uint64_t seed = 1; seed <= 3; ++seed) {
            SCOPED_TRACE("seed " + std::to_string(seed));
            std::mt19937_64 rng(seed);
            Database db;
            QueryCompiler::compileAndExecute("create table t ( a integer not null , b integer not null );", db);
            Table & table = *db.getTable("t");

            struct Branch { std::string name; branch_id_t id; std::set<int32_t> present; };
            std::vector<Branch> branches = {{"master", master_branch_id, {}}};
            auto context = [&](const Branch & branch) {
                auto ctx = std::make_unique<QueryContext>(db);
                ctx->executionContext.branchId = branch.id;
                db.constructBranchLineage(branch.id, ctx->executionContext);
                return ctx;
            };
            {
                auto ctx = context(branches[0]);
                for (int32_t a = 0; a < n; ++a) {
                    auto tuple = makeTuple(table, a);
                    insert_tuple(tuple, table, *ctx);
                    branches[0].present.insert(a);
                }
            }

            for (int step = 0; step < 400; ++step) {
                SCOPED_TRACE("step " + std::to_string(step));
                size_t bi = rng() % branches.size();
                int op = rng() % 100;
                if (op < 15 && branches.size() < 40) {
                    Branch child = branches[bi];
                    child.name = "b" + std::to_string(branches.size());
                    QueryCompiler::compileAndExecute("create branch " + child.name + " from " + branches[bi].name + ";", db);
                    child.id = db._branchMapping[child.name];
                    branches.push_back(std::move(child));
                } else if (op < 80) {
                    // a burst of deletes; tuples that are not in the branch (or were inserted by an
                    // ancestor after the fork) stay out
                    auto ctx = context(branches[bi]);
                    size_t count = 1 + rng() % 120;
                    for (size_t i = 0; i < count; ++i) {
                        tid_t tid = rng() % table.size();
                        delete_tuple(tid, table, *ctx);
                        branches[bi].present.erase(static_cast<int32_t>(tid));
                    }
                } else {
                    auto ctx = context(branches[bi]);
                    size_t count = 1 + rng() % 20;
                    for (size_t i = 0; i < count; ++i) {
                        int32_t a = static_cast<int32_t>(table.size());
                        auto tuple = makeTuple(table, a);
                        insert_tuple(tuple, table, *ctx);
                        branches[bi].present.insert(a);
                    }
                }
                if (step % 40 == 39) {
                    for (auto & branch : branches) {
                        scanned.clear();
                        QueryCompiler::compileAndExecute("select a from t version " + branch.name + ";", db, (void *) &collect);
                        std::string missing, extra;
                        for (int32_t a : branch.present) if (scanned.count(a) == 0) missing += " " + std::to_string(a);
                        for (int32_t a : scanned) if (branch.present.count(a) == 0) extra += " " + std::to_string(a);
                        ASSERT_TRUE(missing.empty() && extra.empty()) << "branch " << branch.name << ": missing" << missing.substr(0, 80)
                                                                     << ", extra" << extra.substr(0, 80);
                    }
                }
            }
            entries += table.getMembership().entries();
        }
#if LAZY_MEMBERSHIP
        EXPECT_GT(entries, 0u);
#endif
    }

}
