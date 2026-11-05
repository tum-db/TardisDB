// Branch-local schema changes: ALTER TABLE ... VERSION b ADD COLUMN / DROP COLUMN

#include "codegen/LLVMCompat.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <random>
#include <set>

#include "algebra/logical/operators.hpp"
#include "foundations/Parallel.hpp"
#include "foundations/version_management.hpp"
#include "native/sql/SqlTuple.hpp"
#include "queryCompiler/queryCompiler.hpp"
#include <gtest/gtest.h>

namespace {

    std::vector<std::string> collected;

    void collect(Native::Sql::SqlTuple * tuple) {
        collected.push_back(Native::Sql::toString(*tuple));
    }

    class SchemaTest : public ::testing::Test {
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

        /// \returns The rows of the query, which must be the same with one thread and with parallel scans
        std::vector<std::string> scan(const std::string & sql) {
            const size_t threads = Parallel::threads, morselSize = Parallel::morselSize;
            auto rows = query(sql);
            Parallel::threads = 4;
            Parallel::morselSize = 2;
            auto parallel = query(sql);
            Parallel::threads = threads;
            Parallel::morselSize = morselSize;
            EXPECT_EQ(rows, parallel) << sql;
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

    TEST_F(SchemaTest, AddedColumnBelongsToTheBranch) {
        createTable();
        exec("create branch b from master;");
        exec("create branch c from master;");
        exec("alter table t version b add column z integer default 7;");

        EXPECT_EQ(scan("select k , z from t version b;"), Rows({"(1, 7)", "(2, 7)", "(3, 7)"}));
        EXPECT_EQ(scan("select * from t version b;"), Rows({"(1, 10, 100, 7)", "(2, 20, 200, 7)", "(3, 30, 300, 7)"}));
        EXPECT_EQ(scan("select * from t;"), Rows({"(1, 10, 100)", "(2, 20, 200)", "(3, 30, 300)"}));
        EXPECT_ANY_THROW(exec("select z from t;"));
        EXPECT_ANY_THROW(exec("select z from t version c;"));
        EXPECT_ANY_THROW(exec("update t version c set z = 1;"));

        // a fork inherits the schema; branches forked before do not get later changes
        exec("create branch d from b;");
        exec("alter table t version b add column w integer default 1;");
        EXPECT_EQ(scan("select k , z from t version d;"), Rows({"(1, 7)", "(2, 7)", "(3, 7)"}));
        EXPECT_ANY_THROW(exec("select w from t version d;"));
        EXPECT_EQ(scan("select k , w from t version b;"), Rows({"(1, 1)", "(2, 1)", "(3, 1)"}));
    }

    TEST_F(SchemaTest, AlterTableErrors) {
        createTable();
        exec("create branch b from master;");
        EXPECT_ANY_THROW(exec("alter table t version b add column z integer;"));  // no default
        EXPECT_ANY_THROW(exec("alter table t version b add column x integer default 0;"));  // exists
        EXPECT_ANY_THROW(exec("alter table t version b add column z nothing default 0;"));
        EXPECT_ANY_THROW(exec("alter table t version b add column z integer default abc;"));
        EXPECT_ANY_THROW(exec("alter table t version b add column z longinteger default 0;"));
        EXPECT_ANY_THROW(exec("alter table t version b drop column z;"));
        EXPECT_ANY_THROW(exec("alter table u version b add column z integer default 0;"));
        EXPECT_ANY_THROW(exec("alter table t version e add column z integer default 0;"));
        EXPECT_EQ(scan("select * from t version b;"), Rows({"(1, 10, 100)", "(2, 20, 200)", "(3, 30, 300)"}));
    }

    TEST_F(SchemaTest, WritesOfTheBranch) {
        createTable();
        exec("create branch b from master;");
        exec("alter table t version b add z integer not null default 7;");
        exec("update t version b set z = z + 1 where k = 1;");
        exec("update t version b set x = 0 where k = 2;");
        exec("insert into t version b ( k , x , y , z ) values ( 4 , 40 , 400 , 44 );");
        exec("insert into t version b ( z , k , x , y ) values ( 55 , 5 , 50 , 500 );");
        exec("insert into t version b ( k , x , y ) values ( 6 , 60 , 600 );");
        exec("insert into t version b values ( 7 , 70 , 700 , 77 );");
        const Rows rows = {"(1, 10, 8)", "(2, 0, 7)", "(3, 30, 7)", "(4, 40, 44)", "(5, 50, 55)", "(6, 60, 7)", "(7, 70, 77)"};
        EXPECT_EQ(scan("select k , x , z from t version b;"), rows);
        EXPECT_EQ(scan("select k from t version b where z = 7;"), Rows({"(2)", "(3)", "(6)"}));

        // main does not have the column, and its writes keep the values of the branch
        exec("update t set x = 1 where k = 3;");
        exec("update t set y = 2 where k = 1;");
        exec("insert into t values ( 8 , 80 , 800 );");
        EXPECT_EQ(scan("select * from t;"), Rows({"(1, 10, 2)", "(2, 20, 200)", "(3, 1, 300)", "(8, 80, 800)"}));
        EXPECT_EQ(scan("select k , x , z from t version b;"), rows);
        EXPECT_ANY_THROW(exec("insert into t values ( 9 , 90 , 900 , 99 );"));
    }

    TEST_F(SchemaTest, VersionsWrittenBeforeTheColumnReadTheDefault) {
        createTable();
        exec("create branch b from master;");
        exec("update t version b set x = 11 where k = 1;"); // a version of b without the column
        exec("update t set x = 12 where k = 2;");           // b reads the old version of main from the chain
        exec("alter table t version b add column z integer default 5;");
        const Rows before = {"(1, 11, 5)", "(2, 20, 5)", "(3, 30, 5)"};
        EXPECT_EQ(scan("select k , x , z from t version b;"), before);

        // the version of b is too small to be overwritten
        exec("update t version b set z = 6 where k = 1;");
        exec("update t version b set z = 8 where k = 1;");
        EXPECT_EQ(scan("select k , x , z from t version b;"), Rows({"(1, 11, 8)", "(2, 20, 5)", "(3, 30, 5)"}));

        // the same through a version map
        exec("create branch c from b;");
        version_map_after_scans = 1;
        EXPECT_EQ(scan("select k , x , z from t version c;"), Rows({"(1, 11, 8)", "(2, 20, 5)", "(3, 30, 5)"}));
        EXPECT_EQ(scan("select k , x , z from t version c;"), Rows({"(1, 11, 8)", "(2, 20, 5)", "(3, 30, 5)"}));
        exec("update t version c set z = 9 where k = 3;");
        EXPECT_EQ(scan("select k , x , z from t version c;"), Rows({"(1, 11, 8)", "(2, 20, 5)", "(3, 30, 9)"}));
        version_map_after_scans = 0;
        EXPECT_EQ(db->getTable("t")->_versionMaps.count(db->_branchMapping.at("c")), 1u);
    }

    TEST_F(SchemaTest, BranchesHaveDifferentColumnsOfTheSameName) {
        createTable();
        exec("create branch b from master;");
        exec("create branch c from master;");
        exec("alter table t version b add column tier integer default 1;");
        exec("alter table t version c add column tier integer default 2;");
        exec("update t version b set tier = 10 where k = 1;");
        exec("update t version c set tier = 20 where k = 2;");
        EXPECT_EQ(scan("select k , tier from t version b;"), Rows({"(1, 10)", "(2, 1)", "(3, 1)"}));
        EXPECT_EQ(scan("select k , tier from t version c;"), Rows({"(1, 2)", "(2, 20)", "(3, 2)"}));

        // queries over several branches need the same column in all of them
        EXPECT_ANY_THROW(exec("select branch , sum ( tier ) from t version ( b , c ) group by branch;"));
        EXPECT_EQ(query("select branch , sum ( x ) from t version ( b , c ) group by branch;"), Rows({"(b, 60)", "(c, 60)"}));
        // DIFF compares the columns both branches have: the tiers are different columns
        EXPECT_EQ(query("select * from diff ( t version b , t version c );"), Rows({}));
        exec("update t version c set x = 21 where k = 2;");
        EXPECT_EQ(query("select * from diff ( t version b , t version c );"), Rows({"(b, 2, 20, 200)", "(c, 2, 21, 200)"}));
        EXPECT_ANY_THROW(exec("select tier from diff ( t version b , t version c );"));
    }

    TEST_F(SchemaTest, AggregatesOverBranchesWithTheColumn) {
        createTable();
        exec("create branch b from master;");
        exec("update t version b set x = 21 where k = 2;"); // without the column
        exec("alter table t version b add column z integer default 5;");
        exec("create branch c from b;");
        exec("update t version c set z = 1 where k = 1;");
        exec("update t version b set z = 2 where k = 3;");
        const Rows expected = {"(b, 12)", "(c, 11)"};
        for (auto strategy : {Algebra::Logical::BranchAggregation::Strategy::Shared,
                              Algebra::Logical::BranchAggregation::Strategy::PerBranch}) {
            Algebra::Logical::BranchAggregation::_strategy = strategy;
            EXPECT_EQ(scan("select branch , sum ( z ) from t version ( b , c ) group by branch;"), expected);
            EXPECT_EQ(scan("select branch , count ( * ) from t version all descendants of b where z = 5 group by branch;"),
                      Rows({"(c, 2)"}));
        }
        Algebra::Logical::BranchAggregation::_strategy = Algebra::Logical::BranchAggregation::Strategy::Shared;
        EXPECT_EQ(query("select branch , k , z from diff ( t version b , t version c );"), Rows({"(b, 1, 5)", "(b, 3, 2)", "(c, 1, 1)", "(c, 3, 5)"}));
    }

    TEST_F(SchemaTest, DropColumn) {
        createTable();
        exec("create branch b from master;");
        exec("alter table t version b drop column y;");
        EXPECT_EQ(scan("select * from t version b;"), Rows({"(1, 10)", "(2, 20)", "(3, 30)"}));
        EXPECT_ANY_THROW(exec("select y from t version b;"));
        EXPECT_EQ(scan("select * from t;"), Rows({"(1, 10, 100)", "(2, 20, 200)", "(3, 30, 300)"}));

        exec("insert into t version b values ( 4 , 40 );");
        exec("update t version b set x = 0 where k = 1;");
        exec("create branch c from b;");
        EXPECT_EQ(scan("select * from t version c;"), Rows({"(1, 0)", "(2, 20)", "(3, 30)", "(4, 40)"}));

        // a column added under the name of a dropped one is a new column
        exec("alter table t version b add column y integer default 0;");
        EXPECT_EQ(scan("select k , y from t version b;"), Rows({"(1, 0)", "(2, 0)", "(3, 0)", "(4, 0)"}));
        exec("alter table t version b drop column y;");
        exec("alter table t version b drop column x;");
        EXPECT_ANY_THROW(exec("alter table t version b drop column k;")); // the last one
    }

    TEST_F(SchemaTest, ColumnOfMain) {
        createTable();
        exec("create branch b from master;");
        exec("alter table t add column w integer default 3;");
        exec("update t set w = 4 where k = 1;");
        EXPECT_EQ(scan("select k , w from t;"), Rows({"(1, 4)", "(2, 3)", "(3, 3)"}));
        EXPECT_ANY_THROW(exec("select w from t version b;"));
        exec("create branch c from master;");
        exec("update t set w = 5 where k = 2;");
        EXPECT_EQ(scan("select k , w from t version c;"), Rows({"(1, 4)", "(2, 3)", "(3, 3)"}));
        EXPECT_EQ(scan("select k , w from t;"), Rows({"(1, 4)", "(2, 5)", "(3, 3)"}));
    }

    TEST_F(SchemaTest, IndexOnAnAddedColumn) {
        createTable();
        exec("create branch b from master;");
        exec("alter table t version b add column z integer default 0;");
        exec("update t version b set z = 1 where k = 2;");
        exec("create index on t ( z ) version b;");
        exec("insert into t version b values ( 4 , 40 , 400 , 1 );");
        EXPECT_EQ(scan("select k from t version b where z = 1;"), Rows({"(2)", "(4)"}));
        EXPECT_EQ(scan("select k from t version b where z = 0;"), Rows({"(1)", "(3)"}));
    }

    TEST_F(SchemaTest, MergeTakesOverAddedColumns) {
        createTable();
        exec("create branch b from master;");
        exec("alter table t version b add column tier integer default 0;");
        exec("update t version b set tier = x where k = 1;");
        exec("update t version b set tier = x where k = 3;");
        exec("update t set y = 1 where k = 3;");
        exec("merge branch b into master;");
        EXPECT_EQ(scan("select k , y , tier from t;"), Rows({"(1, 100, 10)", "(2, 200, 0)", "(3, 1, 30)"}));
        exec("create branch c from master;");
        EXPECT_EQ(scan("select k , tier from t version c;"), Rows({"(1, 10)", "(2, 0)", "(3, 30)"}));

        // the next merge of b takes its new writes of the column
        exec("update t version b set tier = 2 where k = 2;");
        exec("merge b into main;");
        EXPECT_EQ(scan("select k , tier from t;"), Rows({"(1, 10)", "(2, 2)", "(3, 30)"}));
    }

    TEST_F(SchemaTest, MergeAlongTheTree) {
        createTable();
        exec("create branch b1 from master;");
        exec("create branch b2 from b1;");
        exec("alter table t version b2 add column z integer default 1;");
        exec("update t version b2 set z = 2 where k = 2;");
        exec("merge branch b2 into b1;");
        EXPECT_EQ(scan("select k , z from t version b1;"), Rows({"(1, 1)", "(2, 2)", "(3, 1)"}));
        EXPECT_ANY_THROW(exec("select z from t;"));
        exec("merge branch b1 into master;");
        EXPECT_EQ(scan("select k , z from t;"), Rows({"(1, 1)", "(2, 2)", "(3, 1)"}));
    }

    TEST_F(SchemaTest, MergeConflictsOnColumnsAddedInBoth) {
        createTable();
        exec("create branch b from master;");
        exec("alter table t version b add column tier integer default 0;");
        exec("alter table t add column tier integer default 0;");
        EXPECT_ANY_THROW(exec("merge branch b into master;"));

        // a column the target dropped is not added back
        exec("create branch c from master;");
        exec("update t version c set y = 7 where k = 1;");
        exec("alter table t drop column y;");
        exec("merge branch c into master;");
        EXPECT_ANY_THROW(exec("select y from t;"));
        EXPECT_EQ(scan("select k , x from t;"), Rows({"(1, 10)", "(2, 20)", "(3, 30)"}));
    }

    TEST_F(SchemaTest, DropBranchForgetsItsSchema) {
        createTable();
        exec("create branch b from master;");
        exec("alter table t version b add column z integer default 0;");
        exec("drop branch b;");
        exec("create branch b from master;");
        EXPECT_ANY_THROW(exec("select z from t version b;"));
        EXPECT_EQ(scan("select * from t version b;"), Rows({"(1, 10, 100)", "(2, 20, 200)", "(3, 30, 300)"}));
    }


    // Randomised comparison with a model in which every tuple has a value for every column of the table
    // (missing: the default of the column), a write keeps the values of the columns its branch does not
    // have, and a fork copies the schema

    struct ModelColumn {
        std::string name;
        int64_t defaultValue;
        int addedBy;   // -1: CREATE TABLE
        uint64_t time; // of the ALTER TABLE
    };

    using Value = std::map<int, int64_t>; // column -> value
    using State = std::optional<Value>;   // nullopt: not in the branch

    struct SchemaBranch {
        std::string name;
        int parent;
        uint64_t fork;
        bool alive = true;
        std::vector<int> schema;
        std::map<int, std::vector<std::pair<uint64_t, State>>> history; // per key: what the branch wrote when
    };

    struct SchemaModel {
        std::vector<ModelColumn> columns = {{"k", 0, -1, 0}, {"x", 0, -1, 0}, {"y", 0, -1, 0}};
        std::vector<SchemaBranch> branches = {{"master", -1, 0, true, {0, 1, 2}}};
        std::map<std::tuple<int, int, int>, Value> mergeBases; // (src, dst, key)
        std::set<int> keys;
        uint64_t now = 1;

        int64_t get(const Value & v, int column) const {
            auto it = v.find(column);
            return (it != v.end()) ? it->second : columns[column].defaultValue;
        }

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

        int find(int b, const std::string & name) const {
            for (int column : branches[b].schema) if (columns[column].name == name) return column;
            return -1;
        }

        /// whether the branch inherited the column at some time: its own, or added in an ancestor before
        /// the fork towards the branch
        bool had(int b, int column) const {
            const auto & c = columns[column];
            if (c.addedBy < 0) return true;
            auto p = path(b);
            for (size_t i = 0; i < p.size(); ++i) {
                if (p[i] != c.addedBy) continue;
                return i == 0 || branches[p[i - 1]].fork > c.time;
            }
            return false;
        }

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

        bool merge(int src, int dst) {
            std::vector<int> added;
            for (int column : branches[src].schema) {
                auto & target = branches[dst].schema;
                if (std::find(target.begin(), target.end(), column) != target.end() || had(dst, column)) continue;
                if (find(dst, columns[column].name) >= 0) return false;
                added.push_back(column);
            }
            struct Action { int key; State value; bool record; Value merged; bool eraseBase; };
            std::vector<Action> actions;
            for (int key : keys) {
                State s = current(src, key), d = current(dst, key);
                if (!s && !d) continue;
                auto recorded = mergeBases.find({src, dst, key});
                State base = (recorded != mergeBases.end()) ? State(recorded->second) : commonBase(src, dst, key);
                if (s && d) {
                    Value merged;
                    bool changed = false;
                    for (int i = 0; i < int(columns.size()); ++i) {
                        int64_t sv = get(*s, i), dv = get(*d, i);
                        if (sv == dv || (base && get(*base, i) == sv)) merged[i] = dv;
                        else if (base && get(*base, i) == dv) { merged[i] = sv; changed = true; }
                        else return false;
                    }
                    actions.push_back({key, changed ? State(merged) : std::nullopt, true, *s, false});
                } else if (s) {
                    if (!base) actions.push_back({key, s, true, *s, false});
                    else {
                        for (int i = 0; i < int(columns.size()); ++i) if (get(*s, i) != get(*base, i)) return false;
                    }
                } else if (base) {
                    for (int i = 0; i < int(columns.size()); ++i) if (get(*d, i) != get(*base, i)) return false;
                    actions.push_back({key, std::nullopt, false, {}, true});
                }
            }
            for (auto & action : actions) {
                if (action.eraseBase) {
                    write(dst, action.key, std::nullopt);
                    mergeBases.erase({src, dst, action.key});
                    continue;
                }
                if (action.value) write(dst, action.key, action.value);
                if (action.record) mergeBases[{src, dst, action.key}] = action.merged;
            }
            for (int column : added) branches[dst].schema.push_back(column);
            return true;
        }

        std::string row(const std::vector<int> & schema, const Value & v, const std::string & prefix = "") const {
            std::string result = "(" + prefix;
            for (size_t i = 0; i < schema.size(); ++i) result += (i > 0 || !prefix.empty() ? ", " : "") + std::to_string(get(v, schema[i]));
            return result + ")";
        }

        std::vector<std::string> rows(int b) const {
            std::vector<std::string> result;
            for (int key : keys) {
                State s = current(b, key);
                if (s) result.push_back(row(branches[b].schema, *s));
            }
            std::sort(result.begin(), result.end());
            return result;
        }

        /// DIFF compares and returns the columns both branches have
        std::vector<std::string> diff(int a, int b) const {
            std::vector<int> common;
            for (int column : branches[a].schema) {
                auto & other = branches[b].schema;
                if (std::find(other.begin(), other.end(), column) != other.end()) common.push_back(column);
            }
            std::vector<std::string> result;
            for (int key : keys) {
                State sa = current(a, key), sb = current(b, key);
                if (!sa && !sb) continue;
                if (sa && sb && row(common, *sa) == row(common, *sb)) continue;
                for (auto [branch, s] : {std::make_pair(a, sa), std::make_pair(b, sb)}) {
                    if (s) result.push_back(row(common, *s, branches[branch].name));
                }
            }
            std::sort(result.begin(), result.end());
            return result;
        }
    };

    TEST_F(SchemaTest, RandomisedAgainstModel) {
        for (unsigned seed : {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u}) {
            SCOPED_TRACE("seed " + std::to_string(seed));
            db = std::make_unique<Database>();
            exec("create table t ( k integer not null , x integer not null , y integer not null );");
            SchemaModel model;
            std::mt19937 rng(seed);
            auto pick = [&](size_t n) { return std::uniform_int_distribution<size_t>(0, n - 1)(rng); };
            const std::vector<std::string> names = {"z", "w", "y", "x"}; // also those of dropped columns
            int nextKey = 1, nextName = 1;
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

            for (int step = 0; step < 300; ++step) {
                SCOPED_TRACE("step " + std::to_string(step));
                auto branches = live();
                const int b = branches[pick(branches.size())];
                auto & schema = model.branches[b].schema;
                const size_t op = pick(100);
                if (op < 20 || present(b).empty()) {
                    // the columns in a random order, some left out (they take their defaults)
                    int key = nextKey++;
                    Value v = {{0, key}};
                    std::vector<int> given = {0};
                    for (size_t i = 1; i < schema.size(); ++i) {
                        if (pick(4) == 0) continue;
                        v[schema[i]] = pick(1000);
                        given.push_back(schema[i]);
                    }
                    std::shuffle(given.begin(), given.end(), rng);
                    std::string columns, values;
                    for (int column : given) {
                        columns += (columns.empty() ? "" : " , ") + model.columns[column].name;
                        values += (values.empty() ? "" : " , ") + std::to_string(v[column]);
                    }
                    exec("insert into t" + version(b) + " ( " + columns + " ) values ( " + values + " );");
                    model.keys.insert(key);
                    model.write(b, key, v);
                } else if (op < 50 && schema.size() > 1) {
                    auto keys = present(b);
                    int key = keys[pick(keys.size())];
                    Value v = *model.current(b, key);
                    int column = schema[1 + pick(schema.size() - 1)];
                    int64_t value = pick(1000);
                    exec("update t" + version(b) + " set " + model.columns[column].name + " = " + std::to_string(value) +
                         " where k = " + std::to_string(key) + ";");
                    v[column] = value;
                    model.write(b, key, v);
                } else if (op < 57) {
                    auto keys = present(b);
                    int key = keys[pick(keys.size())];
                    exec("delete from t" + version(b) + " where k = " + std::to_string(key) + ";");
                    model.write(b, key, std::nullopt);
                } else if (op < 67 && branches.size() < 7) {
                    std::string name = "s" + std::to_string(nextName++);
                    exec("create branch " + name + " from " + model.branches[b].name + ";");
                    model.branches.push_back({name, b, model.now, true, schema});
                } else if (op < 77) {
                    const std::string & name = names[pick(names.size())];
                    if (model.find(b, name) >= 0) continue;
                    int64_t defaultValue = pick(1000);
                    exec("alter table t" + version(b) + " add column " + name + " integer default " + std::to_string(defaultValue) + ";");
                    model.columns.push_back({name, defaultValue, b, model.now});
                    model.branches[b].schema.push_back(int(model.columns.size()) - 1);
                    counts["columns added"] += 1;
                } else if (op < 82 && schema.size() > 1) {
                    int column = schema[1 + pick(schema.size() - 1)];
                    exec("alter table t" + version(b) + " drop column " + model.columns[column].name + ";");
                    schema.erase(std::find(schema.begin(), schema.end(), column));
                    counts["columns dropped"] += 1;
                } else if (op < 95 && branches.size() > 1) {
                    int dst = branches[pick(branches.size())];
                    if (dst == b) continue;
                    bool expected = model.merge(b, dst);
                    bool merged = true;
                    try {
                        QueryCompiler::compileAndExecute("merge branch " + model.branches[b].name + " into " + model.branches[dst].name + ";", *db);
                    } catch (const std::exception &) {
                        merged = false;
                    }
                    ASSERT_EQ(merged, expected) << "merge " << model.branches[b].name << " into " << model.branches[dst].name;
                    counts[merged ? "merges" : "conflicts"] += 1;
                } else if (b != 0) {
                    exec("drop branch " + model.branches[b].name + " cascade;");
                    for (size_t i = 0; i < model.branches.size(); ++i) {
                        for (int p : model.path(i)) if (p == b) model.branches[i].alive = false;
                    }
                    for (auto it = model.mergeBases.begin(); it != model.mergeBases.end(); ) {
                        bool obsolete = !model.branches[std::get<0>(it->first)].alive || !model.branches[std::get<1>(it->first)].alive;
                        it = obsolete ? model.mergeBases.erase(it) : std::next(it);
                    }
                    counts["drops"] += 1;
                }
                model.now += 1;

                for (int c : live()) {
                    ASSERT_EQ(scan("select * from t" + version(c) + ";"), model.rows(c)) << "branch " << model.branches[c].name;
                }
                auto after = live();
                int c1 = after[pick(after.size())], c2 = after[pick(after.size())];
                ASSERT_EQ(query("select * from diff ( t" + version(c1) + " , t" + version(c2) + " );"), model.diff(c1, c2));
                counts["diff rows"] += model.diff(c1, c2).size();
            }
            std::cout << "seed " << seed << ":";
            for (auto & [what, count] : counts) std::cout << " " << count << " " << what << ",";
            std::cout << " " << model.keys.size() << " tuples, " << model.columns.size() << " columns\n";
        }
    }

}
