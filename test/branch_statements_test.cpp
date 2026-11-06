// CREATE BRANCH ... FROM main, SET with expressions, DIFF, MERGE BRANCH, DROP BRANCH and branch-local indexes

#include "codegen/LLVMCompat.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <random>
#include <set>

#include "foundations/version_management.hpp"
#include "native/sql/SqlTuple.hpp"
#include "queryCompiler/queryCompiler.hpp"
#include <gtest/gtest.h>

namespace {

    std::vector<std::string> collected;

    void collect(Native::Sql::SqlTuple * tuple) {
        collected.push_back(Native::Sql::toString(*tuple));
    }

    class BranchStatementTest : public ::testing::Test {
    protected:
        void SetUp() override {
            db = std::make_unique<Database>();
        }

        void exec(const std::string & sql) {
            QueryCompiler::compileAndExecute(sql, *db);
        }

        /// \returns The rows of the query, sorted
        std::vector<std::string> query(const std::string & sql) {
            collected.clear();
            QueryCompiler::compileAndExecute(sql, *db, (void *) &collect);
            std::vector<std::string> rows = collected;
            std::sort(rows.begin(), rows.end());
            return rows;
        }

        void createTable() {
            exec("create table t ( k integer not null , x integer not null , y integer not null );");
            for (int k = 1; k <= 3; ++k) {
                exec("insert into t ( k , x , y ) values ( " + std::to_string(k) + " , " + std::to_string(10 * k) + " , " +
                     std::to_string(100 * k) + " );");
            }
        }

        std::unique_ptr<Database> db;
    };

    using Rows = std::vector<std::string>;

    TEST_F(BranchStatementTest, MainIsTheRoot) {
        createTable();
        exec("create branch b from main;");
        exec("update t version main set x = 0 where k = 1;");
        EXPECT_EQ(query("select k , x from t version main;"), Rows({"(1, 0)", "(2, 20)", "(3, 30)"}));
        EXPECT_EQ(query("select k , x from t version b;"), Rows({"(1, 10)", "(2, 20)", "(3, 30)"}));
        EXPECT_ANY_THROW(exec("create branch main from master;"));
    }

    TEST_F(BranchStatementTest, StarKeepsTheColumnOrder) {
        exec("create table w ( c integer not null , a integer not null , b integer not null );");
        exec("insert into w ( c , a , b ) values ( 3 , 1 , 2 );");
        EXPECT_EQ(query("select * from w;"), Rows({"(3, 1, 2)"}));
    }

    TEST_F(BranchStatementTest, UpdateWithExpressions) {
        createTable();
        exec("update t set x = x - 5 where k = 1;");
        EXPECT_EQ(query("select x from t where k = 1;"), Rows({"(5)"}));
        exec("update t set x = x * 2 + k , y = y / 10 where k = 2;");
        EXPECT_EQ(query("select x , y from t where k = 2;"), Rows({"(42, 20)"}));
        exec("update t set x = k - x / 2;"); // '/' binds before '-'
        EXPECT_EQ(query("select k , x from t;"), Rows({"(1, -1)", "(2, -19)", "(3, -12)"}));

        exec("create branch b from main;");
        exec("update t version b set y = y + x + 1 where k = 3;");
        EXPECT_EQ(query("select y from t version b where k = 3;"), Rows({"(289)"}));
        EXPECT_EQ(query("select y from t where k = 3;"), Rows({"(300)"}));
        EXPECT_ANY_THROW(exec("update t set x = z + 1;"));
    }

    TEST_F(BranchStatementTest, Diff) {
        createTable();
        exec("insert into t ( k , x , y ) values ( 4 , 40 , 400 );");
        exec("create branch b from master;");
        exec("update t version b set x = 21 where k = 2;");
        exec("delete from t version b where k = 3;");
        exec("insert into t version b ( k , x , y ) values ( 5 , 50 , 500 );");
        exec("update t version b set x = 10 where k = 1;"); // a new version with the same values
        exec("update t set y = 401 where k = 4;");

        EXPECT_EQ(query("select * from diff ( t version b , t version master );"), Rows({
            "(b, 2, 21, 200)", "(b, 4, 40, 400)", "(b, 5, 50, 500)",
            "(master, 2, 20, 200)", "(master, 3, 30, 300)", "(master, 4, 40, 401)"}));
        EXPECT_EQ(query("select branch , x from diff ( t version b , t ) where k = 2;"), Rows({"(b, 21)", "(master, 20)"}));
        EXPECT_EQ(query("select k from diff ( t version b , t version b );"), Rows({}));
        EXPECT_ANY_THROW(exec("select * from diff ( t version b , u version master );"));
    }

    TEST_F(BranchStatementTest, MergeCombinesAttributes) {
        createTable();
        exec("create branch b from master;");
        exec("update t version b set x = 11 where k = 1;");
        exec("update t set y = 101 where k = 1;");
        exec("delete from t version b where k = 2;");
        exec("insert into t version b ( k , x , y ) values ( 4 , 40 , 400 );");
        exec("insert into t ( k , x , y ) values ( 5 , 50 , 500 );");

        exec("merge branch b into master;");
        EXPECT_EQ(query("select k , x , y from t;"), Rows({"(1, 11, 101)", "(3, 30, 300)", "(4, 40, 400)", "(5, 50, 500)"}));
        EXPECT_EQ(query("select k , x , y from t version b;"), Rows({"(1, 11, 100)", "(3, 30, 300)", "(4, 40, 400)"}));

        // the merged version of b is the base of the next merge: no conflict on x
        exec("update t version b set x = 12 where k = 1;");
        exec("update t set y = 102 where k = 1;");
        exec("merge b into main;");
        EXPECT_EQ(query("select k , x , y from t where k = 1;"), Rows({"(1, 12, 102)"}));
    }

    TEST_F(BranchStatementTest, MergeConflictsChangeNothing) {
        createTable();
        exec("create branch b from master;");
        exec("update t version b set x = 11 where k = 1;");
        exec("update t set x = 12 where k = 1;");
        exec("update t version b set y = 201 where k = 2;"); // a clean change, not applied either
        EXPECT_ANY_THROW(exec("merge branch b into master;"));
        EXPECT_EQ(query("select k , x , y from t;"), Rows({"(1, 12, 100)", "(2, 20, 200)", "(3, 30, 300)"}));

        exec("update t version b set x = 12 where k = 1;"); // the same value on both sides
        exec("merge branch b into master;");
        EXPECT_EQ(query("select k , x , y from t;"), Rows({"(1, 12, 100)", "(2, 20, 201)", "(3, 30, 300)"}));
    }

    TEST_F(BranchStatementTest, MergeDetectsDeleteChangeConflicts) {
        createTable();
        exec("create branch b from master;");
        exec("delete from t version b where k = 1;");
        exec("update t set x = 0 where k = 1;");
        EXPECT_ANY_THROW(exec("merge branch b into master;"));
        EXPECT_ANY_THROW(exec("merge branch master into b;"));
        exec("update t set x = 10 where k = 1;"); // master back to the base
        exec("merge branch b into master;");
        EXPECT_EQ(query("select k from t;"), Rows({"(2)", "(3)"}));
    }

    TEST_F(BranchStatementTest, MergeAlongTheTree) {
        createTable();
        exec("create branch b1 from master;");
        exec("create branch b2 from b1;");
        exec("update t version b2 set x = 0 where k = 3;");
        exec("insert into t version b2 ( k , x , y ) values ( 7 , 70 , 700 );");
        exec("merge branch b2 into b1;");
        EXPECT_EQ(query("select k , x from t version b1;"), Rows({"(1, 10)", "(2, 20)", "(3, 0)", "(7, 70)"}));
        EXPECT_EQ(query("select k , x from t;"), Rows({"(1, 10)", "(2, 20)", "(3, 30)"}));
        exec("merge branch b1 into master;");
        EXPECT_EQ(query("select k , x from t;"), Rows({"(1, 10)", "(2, 20)", "(3, 0)", "(7, 70)"}));
        EXPECT_EQ(query("select branch , count ( * ) from t version all group by branch;"), Rows({"(b1, 4)", "(b2, 4)", "(master, 4)"}));
    }

    TEST_F(BranchStatementTest, DropBranch) {
        createTable();
        exec("create branch b1 from master;");
        exec("create branch b2 from b1;");
        exec("create branch c from master;");
        exec("update t version b1 set x = 1 where k = 1;");
        exec("update t version b2 set x = 2 where k = 1;");
        exec("insert into t version b2 ( k , x , y ) values ( 8 , 80 , 800 );");
        EXPECT_ANY_THROW(exec("drop branch b1;"));
        EXPECT_ANY_THROW(exec("drop branch master;"));

        exec("drop branch b1 cascade;");
        EXPECT_EQ(db->_branches.size(), 2u);
        EXPECT_ANY_THROW(exec("select k from t version b2;"));
        EXPECT_EQ(query("select branch , sum ( x ) from t version all group by branch;"), Rows({"(c, 60)", "(master, 60)"}));
        for (auto & entry : db->getTable("t")->_version_mgmt_column) {
            EXPECT_EQ(entry->first, entry.get()); // all versions of b1 and b2 freed
        }

        // names are free again; new branches and tables get bitmap columns of their own
        exec("create branch b1 from c;");
        exec("update t version b1 set x = 3 where k = 1;");
        exec("create table u ( k integer not null );");
        exec("insert into u version b1 ( k ) values ( 1 );");
        EXPECT_EQ(query("select k , x from t version b1 where k = 1;"), Rows({"(1, 3)"}));
        EXPECT_EQ(query("select k , x from t version c where k = 1;"), Rows({"(1, 10)"}));
        EXPECT_EQ(query("select k from u version b1;"), Rows({"(1)"}));
        EXPECT_EQ(query("select k from u;"), Rows({}));
    }

    TEST_F(BranchStatementTest, BranchLocalIndex) {
        createTable();
        exec("create branch b from master;");
        exec("create index on t ( x ) version b;");
        Table & table = *db->getTable("t");
        ASSERT_EQ(table._indexes.size(), 1u);
        EXPECT_EQ(table._indexes.front()->size(), 3u);
        EXPECT_ANY_THROW(exec("create index on t ( x ) version b;"));

        exec("update t version b set x = 99 where x = 20;"); // found through the index
        exec("insert into t version b ( k , x , y ) values ( 4 , 99 , 400 );");
        exec("delete from t version b where k = 1;");
        exec("update t set x = 99 where k = 3;"); // master has no index
        EXPECT_EQ(query("select k from t version b where x = 99;"), Rows({"(2)", "(4)"}));
        EXPECT_EQ(query("select k from t version b where x = 10;"), Rows({}));
        EXPECT_EQ(query("select k from t version b where x = 20;"), Rows({}));
        EXPECT_EQ(query("select k from t where x = 99;"), Rows({"(3)"}));

        exec("create branch c from master;");
        exec("merge branch c into b;");
        exec("drop branch b;");
        EXPECT_TRUE(table._indexes.empty());
    }

    //-------------------------------------------------------------------------
    // Randomised comparison with a model of the branches

    using Value = std::array<int64_t, 2>; // x, y
    using State = std::optional<Value>;   // nullopt: not in the branch

    TEST_F(BranchStatementTest, MergePoliciesResolveConflicts) {
        // an attribute changed on both sides, beside a clean change
        for (const std::string side : {"target", "source"}) {
            SCOPED_TRACE(side);
            db = std::make_unique<Database>();
            createTable();
            exec("create branch b from master;");
            exec("update t version b set x = 11 where k = 1;");
            exec("update t set x = 12 where k = 1;");
            exec("update t version b set y = 201 where k = 2;");
            auto result = QueryCompiler::execute("merge branch b into master on conflict keep " + side + ";", *db, "master",
                                                 [](Native::Sql::SqlTuple *) { });
            EXPECT_EQ(result.notice, "1 conflict kept the " + side);
            EXPECT_EQ(query("select k , x , y from t;"),
                      Rows({side == "target" ? "(1, 12, 100)" : "(1, 11, 100)", "(2, 20, 201)", "(3, 30, 300)"}));
            // the version of b is the base of the next merge: the conflict does not come back
            exec("update t version b set y = 202 where k = 2;");
            exec("merge branch b into master;");
            EXPECT_EQ(query("select k , x , y from t where k = 2;"), Rows({"(2, 20, 202)"}));
        }
    }

    TEST_F(BranchStatementTest, MergePoliciesResolveDeleteChangeConflicts) {
        // deleted in the source and changed in the target
        createTable();
        exec("create branch b from master;");
        exec("delete from t version b where k = 1;");
        exec("update t set x = 0 where k = 1;");
        exec("merge branch b into master on conflict keep target;");
        EXPECT_EQ(query("select k , x from t;"), Rows({"(1, 0)", "(2, 20)", "(3, 30)"}));
        exec("merge branch b into master on conflict keep source;");
        EXPECT_EQ(query("select k from t;"), Rows({"(2)", "(3)"}));

        // deleted in the target and changed in the source
        db = std::make_unique<Database>();
        createTable();
        exec("create branch c from master;");
        exec("update t version c set x = 31 where k = 3;");
        exec("delete from t where k = 3;");
        exec("merge branch c into master on conflict keep target;");
        EXPECT_EQ(query("select k from t;"), Rows({"(1)", "(2)"}));
        exec("merge branch c into master;"); // its version is the base now: no conflict
        EXPECT_EQ(query("select k from t;"), Rows({"(1)", "(2)"}));
        exec("update t version c set x = 32 where k = 3;");
        exec("merge branch c into master on conflict keep source;");
        EXPECT_EQ(query("select k , x from t;"), Rows({"(1, 10)", "(2, 20)", "(3, 32)"}));
        EXPECT_ANY_THROW(exec("merge branch c into master on conflict keep both;"));
    }

    struct ModelBranch {
        std::string name;
        int parent;
        uint64_t fork;
        bool alive = true;
        std::map<int, std::vector<std::pair<uint64_t, State>>> history; // per key: what the branch wrote when
    };

    struct Model {
        std::vector<ModelBranch> branches = {{"master", -1, 0}};
        std::map<std::tuple<int, int, int>, Value> mergeBases; // (src, dst, key)
        std::set<int> keys;
        uint64_t now = 1;

        State state(int b, int key, uint64_t time) const {
            for (;;) {
                const auto & branch = branches[b];
                auto it = branch.history.find(key);
                if (it != branch.history.end()) {
                    for (auto entry = it->second.rbegin(); entry != it->second.rend(); ++entry) {
                        if (entry->first <= time) return entry->second;
                    }
                }
                if (branch.parent < 0) return std::nullopt;
                time = std::min(time, branch.fork - 1);
                b = branch.parent;
            }
        }

        State current(int b, int key) const { return state(b, key, now); }

        void write(int b, int key, State value) { branches[b].history[key].emplace_back(now, value); }

        std::vector<int> path(int b) const {
            std::vector<int> result;
            for (; b >= 0; b = branches[b].parent) result.push_back(b);
            return result;
        }

        /// the newest version both branches see: the state of their lowest common ancestor when they diverged
        State commonBase(int src, int dst, int key) const {
            auto ps = path(src), pd = path(dst);
            int lca = -1;
            for (int b : ps) if (std::find(pd.begin(), pd.end(), b) != pd.end()) { lca = b; break; }
            auto divergence = [&](const std::vector<int> & p) -> uint64_t {
                if (p.front() == lca) return UINT64_MAX;
                for (size_t i = 1; i < p.size(); ++i) if (p[i] == lca) return branches[p[i - 1]].fork;
                return UINT64_MAX;
            };
            uint64_t t = std::min(divergence(ps), divergence(pd));
            return state(lca, key, t == UINT64_MAX ? now : t - 1);
        }

        /// \returns Whether the merge succeeded; policy 0: fail on conflicts, 1: keep the target, 2: keep the source
        bool merge(int src, int dst, int policy = 0) {
            struct Action { int key; State value; bool record; Value merged; bool eraseBase; };
            std::vector<Action> actions;
            for (int key : keys) {
                State s = current(src, key), d = current(dst, key);
                if (!s && !d) continue;
                auto recorded = mergeBases.find({src, dst, key});
                State base = (recorded != mergeBases.end()) ? State(recorded->second) : commonBase(src, dst, key);
                if (s && d) {
                    Value merged;
                    for (int i = 0; i < 2; ++i) {
                        if ((*s)[i] == (*d)[i] || (base && (*base)[i] == (*s)[i])) merged[i] = (*d)[i];
                        else if (base && (*base)[i] == (*d)[i]) merged[i] = (*s)[i];
                        else if (policy == 0) return false;
                        else merged[i] = (policy == 1) ? (*d)[i] : (*s)[i];
                    }
                    actions.push_back({key, merged != *d ? State(merged) : std::nullopt, true, *s, false});
                } else if (s) {
                    if (!base) actions.push_back({key, s, true, *s, false});
                    else if (*s != *base) {
                        if (policy == 0) return false;
                        actions.push_back({key, policy == 2 ? s : std::nullopt, true, *s, false});
                    }
                } else if (base) {
                    if (*d != *base && policy == 0) return false;
                    if (*d == *base || policy == 2) actions.push_back({key, std::nullopt, false, {}, true});
                }
            }
            for (auto & action : actions) {
                State d = current(dst, action.key);
                if (action.eraseBase) {
                    write(dst, action.key, std::nullopt);
                    mergeBases.erase({src, dst, action.key});
                    continue;
                }
                if (action.value) write(dst, action.key, action.value);
                if (action.record) mergeBases[{src, dst, action.key}] = action.merged;
            }
            return true;
        }

        void drop(int b) {
            for (size_t i = 0; i < branches.size(); ++i) {
                bool inSubtree = false;
                for (int p : path(i)) if (p == b) inSubtree = true;
                if (!inSubtree) continue;
                branches[i].alive = false;
                for (auto it = mergeBases.begin(); it != mergeBases.end(); ) {
                    bool obsolete = std::get<0>(it->first) == int(i) || std::get<1>(it->first) == int(i);
                    it = obsolete ? mergeBases.erase(it) : std::next(it);
                }
            }
        }

        std::vector<std::string> rows(int b, std::optional<int64_t> x = std::nullopt) const {
            std::vector<std::string> result;
            for (int key : keys) {
                State s = current(b, key);
                if (!s || (x && (*s)[0] != *x)) continue;
                result.push_back("(" + std::to_string(key) + ", " + std::to_string((*s)[0]) + ", " + std::to_string((*s)[1]) + ")");
            }
            std::sort(result.begin(), result.end());
            return result;
        }

        std::vector<std::string> diff(int a, int b) const {
            std::vector<std::string> result;
            for (int key : keys) {
                State sa = current(a, key), sb = current(b, key);
                if (sa == sb) continue;
                for (auto [branch, s] : {std::make_pair(a, sa), std::make_pair(b, sb)}) {
                    if (!s) continue;
                    result.push_back("(" + branches[branch].name + ", " + std::to_string(key) + ", " + std::to_string((*s)[0]) + ", " +
                                     std::to_string((*s)[1]) + ")");
                }
            }
            std::sort(result.begin(), result.end());
            return result;
        }
    };

    TEST_F(BranchStatementTest, RandomisedAgainstModel) {
        for (unsigned seed : {1u, 2u, 3u, 4u, 5u, 6u}) {
            SCOPED_TRACE("seed " + std::to_string(seed));
            db = std::make_unique<Database>();
            exec("create table t ( k integer not null , x integer not null , y integer not null );");
            Model model;
            std::mt19937 rng(seed);
            auto pick = [&](size_t n) { return std::uniform_int_distribution<size_t>(0, n - 1)(rng); };
            int nextKey = 1;
            int nextName = 1;
            std::set<std::pair<int, int>> indexed; // (branch, column 0: k, 1: x)
            std::map<std::string, int> counts;

            auto live = [&]() {
                std::vector<int> result;
                for (size_t b = 0; b < model.branches.size(); ++b) if (model.branches[b].alive) result.push_back(b);
                return result;
            };
            auto present = [&](int b) {
                std::vector<int> result;
                for (int key : model.keys) if (model.current(b, key)) result.push_back(key);
                return result;
            };
            auto version = [&](int b) { return std::string(" version ") + model.branches[b].name; };

            for (int step = 0; step < 250; ++step) {
                SCOPED_TRACE("step " + std::to_string(step));
                auto branches = live();
                const int b = branches[pick(branches.size())];
                const size_t op = pick(100);
                if (op < 25 || present(b).empty()) {
                    int key = nextKey++;
                    Value v = {int64_t(pick(5)), int64_t(pick(1000))};
                    exec("insert into t" + version(b) + " ( k , x , y ) values ( " + std::to_string(key) + " , " +
                         std::to_string(v[0]) + " , " + std::to_string(v[1]) + " );");
                    model.keys.insert(key);
                    model.write(b, key, v);
                } else if (op < 55) {
                    auto keys = present(b);
                    int key = keys[pick(keys.size())];
                    Value v = *model.current(b, key);
                    if (pick(2) == 0) {
                        int64_t delta = int64_t(pick(3)) - 1;
                        exec("update t" + version(b) + " set x = x + " + std::to_string(delta) + " where k = " + std::to_string(key) + ";");
                        v[0] += delta;
                    } else {
                        v[1] = pick(1000);
                        exec("update t" + version(b) + " set y = " + std::to_string(v[1]) + " where k = " + std::to_string(key) + ";");
                    }
                    model.write(b, key, v);
                } else if (op < 65) {
                    auto keys = present(b);
                    int key = keys[pick(keys.size())];
                    exec("delete from t" + version(b) + " where k = " + std::to_string(key) + ";");
                    model.write(b, key, std::nullopt);
                } else if (op < 77 && branches.size() < 7) {
                    std::string name = "r" + std::to_string(nextName++);
                    exec("create branch " + name + " from " + model.branches[b].name + ";");
                    model.branches.push_back({name, b, model.now});
                } else if (op < 90 && branches.size() > 1) {
                    int dst = branches[pick(branches.size())];
                    if (dst == b) continue;
                    const int policy = pick(3);
                    bool expected = model.merge(b, dst, policy);
                    bool merged = true;
                    try {
                        QueryCompiler::compileAndExecute("merge branch " + model.branches[b].name + " into " + model.branches[dst].name +
                                                         (policy == 0 ? "" : policy == 1 ? " on conflict keep target" : " on conflict keep source") + ";", *db);
                    } catch (const std::exception &) {
                        merged = false;
                    }
                    ASSERT_EQ(merged, expected) << "merge " << model.branches[b].name << " into " << model.branches[dst].name;
                    counts[merged ? "merges" : "conflicts"] += 1;
                } else if (op < 94 && b != 0) {
                    exec("drop branch " + model.branches[b].name + " cascade;");
                    model.drop(b);
                    counts["drops"] += 1;
                } else {
                    int column = pick(2);
                    if (indexed.insert({b, column}).second) {
                        exec(std::string("create index on t ( ") + (column == 0 ? "k" : "x") + " )" + version(b) + ";");
                        counts["indexes"] += 1;
                    }
                }
                model.now += 1;

                // every branch, a difference and a lookup by x
                for (int c : live()) {
                    ASSERT_EQ(query("select k , x , y from t" + version(c) + ";"), model.rows(c)) << "branch " << model.branches[c].name;
                }
                auto after = live();
                int c1 = after[pick(after.size())], c2 = after[pick(after.size())];
                ASSERT_EQ(query("select * from diff ( t" + version(c1) + " , t" + version(c2) + " );"), model.diff(c1, c2));
                int64_t x = pick(5);
                ASSERT_EQ(query("select k , x , y from t" + version(c1) + " where x = " + std::to_string(x) + ";"), model.rows(c1, x));
                counts["index lookups"] += indexed.count({c1, 1});
                counts["diff rows"] += model.diff(c1, c2).size();
            }
            std::cout << "seed " << seed << ":";
            for (auto & [what, count] : counts) std::cout << " " << count << " " << what << ",";
            std::cout << " " << model.keys.size() << " tuples\n";
        }
    }

}
