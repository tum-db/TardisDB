// Statements from several threads at once: every worker writes its own branch (two workers each the
// same tuples), main is written meanwhile, branches are created and dropped, and an aggregate reads
// several branches; after every statement, each thread checks the sum of its branch against what it wrote.
// Main also writes in place beside readers and writers of other branches, and inserts run beside them

#include "codegen/LLVMCompat.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <thread>

#include "foundations/Parallel.hpp"
#include "foundations/version_management.hpp"
#include "native/sql/SqlTuple.hpp"
#include "queryCompiler/queryCompiler.hpp"
#include <gtest/gtest.h>

namespace {

    // aggregates call back on the thread of the query
    thread_local std::string result;
    thread_local std::vector<std::string> rows;

    void collect(Native::Sql::SqlTuple * tuple) {
        result = Native::Sql::toString(*tuple);
        rows.push_back(result);
    }

    int64_t sumOf(Database & db, const std::string & branch) {
        result.clear();
        QueryCompiler::compileAndExecute("select sum(x) from t version " + branch + ";", db, (void *) &collect);
        // "(value)"
        return std::stoll(result.substr(1, result.size() - 2));
    }

    void exec(Database & db, const std::string & sql) {
        QueryCompiler::compileAndExecute(sql, db);
    }

    // scans call back on the workers: the rows of all scans, and those with x other than 0
    std::atomic<size_t> scannedRows(0), changedRows(0);

    void countRow(Native::Sql::SqlTuple * tuple) {
        ++scannedRows;
        if (Native::Sql::toString(*tuple) != "(0)") ++changedRows;
    }

    // writers of different branches record their writes and deletes at the same time, each under its own
    // identifier: the lists and sets of all identifiers exist before (growing them moved them under the others)
    TEST(ConcurrencyTest, WriteListsExistBeforeWrites) {
        Database db;
        exec(db, "create table before ( k integer not null );");
        for (int b = 0; b < 5; ++b) exec(db, "create branch b" + std::to_string(b) + " from main;");
        exec(db, "create table after ( k integer not null );");
        exec(db, "create branch c from b0;");
        for (const char * name : {"before", "after"}) {
            Table & table = *db.getTable(name);
            for (const auto & [id, branch] : db._branches) {
                const ident_t ident = db.getIdent(id);
                EXPECT_LT(ident, table._writes.size()) << name << " " << branch->name;
                EXPECT_LT(ident, table._deleted.size()) << name << " " << branch->name;
            }
        }
    }

    TEST(ConcurrencyTest, BranchesInParallel) {
        constexpr int64_t tuples = 4000, groups = 8, workers = 6, rounds = 20, perGroup = tuples / groups;
        const size_t threads = Parallel::threads, morselSize = Parallel::morselSize;
        Parallel::threads = 4;
        Parallel::morselSize = 256;

        Database db;
        exec(db, "create table t ( k integer not null , g integer not null , x integer not null );");
        {
            Table & table = *db.getTable("t");
            QueryContext ctx(db);
            ctx.executionContext.branchId = master_branch_id;
            db.constructBranchLineage(master_branch_id, ctx.executionContext);
            for (int64_t k = 0; k < tuples; ++k) {
                std::vector<std::unique_ptr<Native::Sql::Value>> values;
                values.push_back(Native::Sql::Value::castString(std::to_string(k), table.getCI("k")->type));
                values.push_back(Native::Sql::Value::castString(std::to_string(k % groups), table.getCI("g")->type));
                values.push_back(Native::Sql::Value::castString("0", table.getCI("x")->type));
                Native::Sql::SqlTuple tuple(std::move(values));
                insert_tuple(tuple, table, ctx);
            }
        }
        exec(db, "create branch still from main;");
        for (int w = 0; w < workers; ++w) exec(db, "create branch w" + std::to_string(w) + " from main;");

        std::mutex failuresLatch;
        std::vector<std::string> failures;
        auto fail = [&](const std::string & message) {
            std::lock_guard<std::mutex> lock(failuresLatch);
            failures.push_back(message);
        };
        auto check = [&](const std::string & who, int64_t got, int64_t expected) {
            if (got != expected) fail(who + ": sum " + std::to_string(got) + ", expected " + std::to_string(expected));
            return got == expected;
        };

        std::vector<std::thread> pool;
        // workers: updates, inserts and deletes in their own branch
        for (int w = 0; w < workers; ++w) {
            pool.emplace_back([&, w] {
                const std::string branch = "w" + std::to_string(w), own = std::to_string(100 + w);
                int64_t expected = 0, inserted = 0;
                try {
                    for (int64_t r = 0; r < rounds; ++r) {
                        // two workers each update the same tuples at the same time
                        exec(db, "update t version " + branch + " set x = x + 1 where g = " + std::to_string((r + w / 2) % groups) + ";");
                        expected += perGroup;
                        exec(db, "insert into t version " + branch + " ( k , g , x ) values ( " + std::to_string(tuples + r) + " , " + own + " , 5 );");
                        expected += 5;
                        ++inserted;
                        if (r % 5 == 4) {
                            exec(db, "delete from t version " + branch + " where g = " + own + ";");
                            expected -= 5 * inserted;
                            inserted = 0;
                        }
                        if (!check(branch, sumOf(db, branch), expected)) return;
                    }
                } catch (const std::exception & e) {
                    fail(branch + ": " + e.what());
                }
            });
        }
        // main: in-place writes to the tuples the workers' branches share with it
        pool.emplace_back([&] {
            try {
                for (int64_t r = 0; r < rounds; ++r) {
                    exec(db, "update t version main set x = x + 1000 where g = " + std::to_string(r % groups) + ";");
                    if (!check("main", sumOf(db, "main"), 1000 * perGroup * (r + 1))) return;
                }
            } catch (const std::exception & e) {
                fail(std::string("main: ") + e.what());
            }
        });
        // forks of main see main between two of its updates; forks of an unchanged branch see it as it is
        pool.emplace_back([&] {
            try {
                for (int64_t r = 0; r < rounds; ++r) {
                    exec(db, "create branch fm from main;");
                    const int64_t sum = sumOf(db, "fm");
                    if (sum % (1000 * perGroup) != 0) fail("fork of main: sum " + std::to_string(sum));
                    exec(db, "update t version fm set x = x + 1 where g = 0;");
                    if (!check("fork of main", sumOf(db, "fm"), sum + perGroup)) return;
                    exec(db, "drop branch fm;");
                    exec(db, "create branch fs from still;");
                    exec(db, "update t version fs set x = x + 2 where g = 1;");
                    if (!check("fork of still", sumOf(db, "fs"), 2 * perGroup)) return;
                    exec(db, "drop branch fs;");
                    if (!check("still", sumOf(db, "still"), 0)) return;
                }
            } catch (const std::exception & e) {
                fail(std::string("forks: ") + e.what());
            }
        });
        // aggregates over several branches see the unchanged branch as it is
        std::atomic<bool> done(false);
        std::thread reader([&] {
            try {
                while (!done) {
                    rows.clear();
                    QueryCompiler::compileAndExecute("select branch, sum(x) from t version ( still , w0 , w1 ) group by branch;", db, (void *) &collect);
                    if (rows.size() != 3 || std::find(rows.begin(), rows.end(), "(still, 0)") == rows.end()) {
                        fail("aggregate: " + std::to_string(rows.size()) + " rows" + (rows.empty() ? "" : ", first " + rows[0]));
                        return;
                    }
                }
            } catch (const std::exception & e) {
                fail(std::string("aggregate: ") + e.what());
            }
        });
        for (auto & thread : pool) thread.join();
        done = true;
        reader.join();
        Parallel::threads = threads;
        Parallel::morselSize = morselSize;

        for (auto & message : failures) ADD_FAILURE() << message;
        EXPECT_EQ(sumOf(db, "still"), 0);
        EXPECT_EQ(sumOf(db, "main"), 1000 * perGroup * rounds);
    }


    // writers of different branches prepend to the same chains at the same time; afterwards every
    // chain holds all versions, and every branch reads its last one
    TEST(ConcurrencyTest, PrependsToSharedChains) {
        constexpr int tuples = 200, writers = 8, rounds = 40;
        const bool overwrite = overwrite_own_versions;
        overwrite_own_versions = false; // every update prepends a version

        Database db;
        exec(db, "create table t ( k integer not null , x integer not null );");
        Table & table = *db.getTable("t");
        auto makeTuple = [&](int64_t k, int64_t x) {
            std::vector<std::unique_ptr<Native::Sql::Value>> values;
            values.push_back(Native::Sql::Value::castString(std::to_string(k), table.getCI("k")->type));
            values.push_back(Native::Sql::Value::castString(std::to_string(x), table.getCI("x")->type));
            return Native::Sql::SqlTuple(std::move(values));
        };
        {
            QueryContext ctx(db);
            ctx.executionContext.branchId = master_branch_id;
            db.constructBranchLineage(master_branch_id, ctx.executionContext);
            for (int k = 0; k < tuples; ++k) {
                auto tuple = makeTuple(k, 0);
                insert_tuple(tuple, table, ctx);
            }
        }
        std::vector<branch_id_t> branches;
        for (int w = 0; w < writers; ++w) {
            exec(db, "create branch w" + std::to_string(w) + " from main;");
            branches.push_back(db._branchMapping.at("w" + std::to_string(w)));
        }
        exec(db, "create branch still from main;");

        std::atomic<bool> failed(false);
        std::vector<std::thread> threads;
        for (int w = 0; w < writers; ++w) {
            threads.emplace_back([&, w] {
                try {
                    QueryContext ctx(db);
                    ctx.executionContext.branchId = branches[w];
                    db.constructBranchLineage(branches[w], ctx.executionContext);
                    for (int r = 1; r <= rounds; ++r) {
                        for (int k = 0; k < tuples; ++k) {
                            auto tuple = makeTuple(k, 1000 * w + r);
                            update_tuple(k, tuple, table, ctx);
                        }
                    }
                } catch (const std::exception &) {
                    failed = true;
                }
            });
        }
        // readers of an unchanged branch walk the growing chains meanwhile
        std::atomic<bool> done(false);
        size_t walks = 0, wrong = 0;
        std::thread reader([&] {
            BranchReader still(db, db._branchMapping.at("still"));
            while (!done) {
                for (tid_t tid = 0; tid < tuples; ++tid) {
                    const VersionEntry * entry = table._version_mgmt_column[tid].get();
                    if (still.visible(entry) != entry) ++wrong;
                    ++walks;
                }
            }
        });
        for (auto & thread : threads) thread.join();
        done = true;
        reader.join();
        overwrite_own_versions = overwrite;

        EXPECT_FALSE(failed);
        EXPECT_EQ(wrong, 0u) << "of " << walks << " walks";
        for (tid_t tid = 0; tid < tuples; ++tid) {
            const VersionEntry * entry = table._version_mgmt_column[tid].get();
            size_t length = 0;
            for (const void * element = chain_head(entry); element != nullptr;
                 element = (element == entry) ? entry->next : static_cast<const VersionedTupleStorage *>(element)->next) {
                ++length;
            }
            ASSERT_EQ(length, 1u + writers * rounds) << "tuple " << tid;
            for (int w = 0; w < writers; ++w) {
                const void * version = BranchReader(db, branches[w]).visible(entry);
                ASSERT_NE(version, entry);
                auto tuple = load_version(tid, table, version);
                ASSERT_EQ(Native::Sql::toString(*tuple), "(" + std::to_string(tid) + ", " + std::to_string(1000 * w + rounds) + ")");
            }
        }
    }


    // main writes in place beside readers of other branches, which go on reading the old versions,
    // and beside writers of other branches, which prepend to the same chains; version maps are built
    // meanwhile. Every write of main moves its in-place version into the chain
    TEST(ConcurrencyTest, MainWritesBesideReaders) {
        constexpr int64_t tuples = 4000, groups = 4, rounds = 40, perGroup = tuples / groups;
        const size_t threads = Parallel::threads, morselSize = Parallel::morselSize, mapAfter = version_map_after_scans;
        const bool overwrite = overwrite_own_versions;
        Parallel::threads = 4;
        Parallel::morselSize = 256;
        constexpr size_t mapAfterScans = 8;
        version_map_after_scans = mapAfterScans; // the readers' maps are built while main writes
        overwrite_own_versions = false; // every write of main moves the in-place version into the chain

        Database db;
        exec(db, "create table t ( k integer not null , g integer not null , x integer not null );");
        {
            Table & table = *db.getTable("t");
            QueryContext ctx(db);
            ctx.executionContext.branchId = master_branch_id;
            db.constructBranchLineage(master_branch_id, ctx.executionContext);
            for (int64_t k = 0; k < tuples; ++k) {
                std::vector<std::unique_ptr<Native::Sql::Value>> values;
                values.push_back(Native::Sql::Value::castString(std::to_string(k), table.getCI("k")->type));
                values.push_back(Native::Sql::Value::castString(std::to_string(k % groups), table.getCI("g")->type));
                values.push_back(Native::Sql::Value::castString("0", table.getCI("x")->type));
                Native::Sql::SqlTuple tuple(std::move(values));
                insert_tuple(tuple, table, ctx);
            }
        }
        for (auto name : {"s0", "s1", "w0", "w1"}) exec(db, std::string("create branch ") + name + " from main;");

        std::mutex failuresLatch;
        std::vector<std::string> failures;
        auto fail = [&](const std::string & message) {
            std::lock_guard<std::mutex> lock(failuresLatch);
            failures.push_back(message);
        };
        auto check = [&](const std::string & who, int64_t got, int64_t expected) {
            if (got != expected) fail(who + ": sum " + std::to_string(got) + ", expected " + std::to_string(expected));
            return got == expected;
        };

        std::atomic<bool> done(false);
        std::vector<std::thread> pool;
        pool.emplace_back([&] {
            try {
                for (int64_t r = 0; r < rounds; ++r) {
                    exec(db, "update t version main set x = x + 1000 where g = " + std::to_string(r % groups) + ";");
                    if (!check("main", sumOf(db, "main"), 1000 * perGroup * (r + 1))) break;
                    if (r % 10 == 5) {
                        exec(db, "create branch fm from main;");
                        check("fork of main", sumOf(db, "fm"), 1000 * perGroup * (r + 1));
                        exec(db, "drop branch fm;");
                    }
                }
            } catch (const std::exception & e) {
                fail(std::string("main: ") + e.what());
            }
            done = true;
        });
        // writers of other branches prepend to the chains of the tuples main writes
        for (int w = 0; w < 2; ++w) {
            pool.emplace_back([&, w] {
                const std::string branch = "w" + std::to_string(w);
                try {
                    for (int64_t r = 0, expected = 0; !done; ++r) {
                        exec(db, "update t version " + branch + " set x = x + 1 where g = " + std::to_string((r + w) % groups) + ";");
                        expected += perGroup;
                        if (!check(branch, sumOf(db, branch), expected)) return;
                    }
                } catch (const std::exception & e) {
                    fail(branch + ": " + e.what());
                }
            });
        }
        // readers of unchanged branches see the versions main has overwritten: scans of s0, which get a
        // version map, and aggregates of s1, which walk the chains
        std::atomic<size_t> scans(0);
        scannedRows = 0;
        changedRows = 0;
        for (int reader = 0; reader < 2; ++reader) {
            pool.emplace_back([&] {
                try {
                    while (!done) {
                        QueryCompiler::compileAndExecute("select x from t version s0;", db, (void *) &countRow);
                        ++scans;
                    }
                } catch (const std::exception & e) {
                    fail(std::string("s0: ") + e.what());
                }
            });
        }
        pool.emplace_back([&] {
            try {
                while (!done) {
                    if (!check("s1", sumOf(db, "s1"), 0)) return;
                }
            } catch (const std::exception & e) {
                fail(std::string("s1: ") + e.what());
            }
        });
        pool.emplace_back([&] {
            try {
                while (!done) {
                    rows.clear();
                    QueryCompiler::compileAndExecute("select branch, sum(x) from t version ( s0 , s1 ) group by branch;", db, (void *) &collect);
                    if (rows.size() != 2 || std::count(rows.begin(), rows.end(), "(s0, 0)") + std::count(rows.begin(), rows.end(), "(s1, 0)") != 2) {
                        fail("aggregate: " + std::to_string(rows.size()) + " rows" + (rows.empty() ? "" : ", first " + rows[0]));
                        return;
                    }
                }
            } catch (const std::exception & e) {
                fail(std::string("aggregate: ") + e.what());
            }
        });
        for (auto & thread : pool) thread.join();
        Parallel::threads = threads;
        Parallel::morselSize = morselSize;
        version_map_after_scans = mapAfter;
        overwrite_own_versions = overwrite;

        for (auto & message : failures) ADD_FAILURE() << message;
        EXPECT_GT(scans, mapAfterScans);
        EXPECT_EQ(scannedRows, scans * tuples);
        EXPECT_EQ(changedRows, 0u);
        Table & table = *db.getTable("t");
        EXPECT_EQ(table._inPlaceWrites, nullptr);
        EXPECT_EQ(table._versionMaps.count(db._branchMapping.at("s0")), 1u);
        EXPECT_EQ(sumOf(db, "main"), 1000 * perGroup * rounds);
        EXPECT_EQ(sumOf(db, "s0"), 0);
        EXPECT_EQ(sumOf(db, "s1"), 0);
        // every chain holds the version entry once and copies of the versions main overwrote (dropping fm
        // collects those that no branch sees); s0 reads the first of them
        BranchReader s0(db, db._branchMapping.at("s0"));
        for (tid_t tid = 0; tid < tuples; ++tid) {
            const VersionEntry * entry = table._version_mgmt_column[tid].get();
            size_t entries = 0, mainCopies = 0;
            for (const void * element = chain_head(entry); element != nullptr; element = chain_next(entry, element)) {
                if (element == entry) {
                    ++entries;
                } else if (static_cast<const VersionedTupleStorage *>(element)->branch_id == master_branch_id) {
                    ++mainCopies;
                }
            }
            ASSERT_EQ(entries, 1u) << "tuple " << tid;
            ASSERT_GE(mainCopies, 1u) << "tuple " << tid;
            ASSERT_LE(mainCopies, size_t(rounds / groups)) << "tuple " << tid;
            const void * version = s0.visible(entry);
            ASSERT_NE(version, entry);
            ASSERT_EQ(Native::Sql::toString(*load_version(tid, table, version)),
                      "(" + std::to_string(tid) + ", " + std::to_string(tid % groups) + ", 0)");
        }
    }


    // the steps of an in-place write of main beside a statement that may read the old version in place:
    // the write copies the version into the chain at once and overwrites it only once the statement has ended
    TEST(ConcurrencyTest, InPlaceWritesWaitForReaders) {
        constexpr int tuples = 100, written = 10;
        Database db;
        exec(db, "create table t ( k integer not null , x integer not null );");
        Table & table = *db.getTable("t");
        auto makeTuple = [&](int64_t k, int64_t x) {
            std::vector<std::unique_ptr<Native::Sql::Value>> values;
            values.push_back(Native::Sql::Value::castString(std::to_string(k), table.getCI("k")->type));
            values.push_back(Native::Sql::Value::castString(std::to_string(x), table.getCI("x")->type));
            return Native::Sql::SqlTuple(std::move(values));
        };
        QueryContext ctx(db);
        ctx.executionContext.branchId = master_branch_id;
        db.constructBranchLineage(master_branch_id, ctx.executionContext);
        for (int k = 0; k < tuples; ++k) {
            auto tuple = makeTuple(k, 0);
            insert_tuple(tuple, table, ctx);
        }
        exec(db, "create branch mapped from main;");
        exec(db, "create branch later from main;");
        const branch_id_t mapped = db._branchMapping.at("mapped"), later = db._branchMapping.at("later");
        build_version_map(table, mapped);

        // a version map whose walk precedes the writes
        auto walked = compute_version_map(table, later);
        // a statement that has entered before the writes, e.g. a scan that has decided to read in place
        const unsigned slot = table._readers.enter();
        InPlaceWrites pending;
        table._inPlaceWrites = &pending;
        for (int k = 0; k < written; ++k) {
            auto tuple = makeTuple(k, 7);
            update_tuple(k, tuple, table, ctx);
        }
        // and is added after them: it names the copies
        add_version_map(table, later, std::move(walked));
        std::atomic<bool> finished(false);
        std::thread writer([&] {
            finish_in_place_writes(table, pending);
            finished = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        EXPECT_FALSE(finished);
        BranchReader reader(db, mapped);
        for (tid_t tid = 0; tid < written; ++tid) {
            const VersionEntry * entry = table._version_mgmt_column[tid].get();
            // the old version, in place and in its copy, which later readers find
            EXPECT_EQ(Native::Sql::toString(*get_current_master(tid, table)), "(" + std::to_string(tid) + ", 0)");
            const void * copy = reader.visible(entry);
            ASSERT_NE(copy, entry);
            EXPECT_EQ(Native::Sql::toString(*load_version(tid, table, copy)), "(" + std::to_string(tid) + ", 0)");
            EXPECT_EQ(table._head_ident[tid], invalid_ident);
            for (branch_id_t branch : {mapped, later}) {
                EXPECT_EQ(table._versionMaps.at(branch)[tid], reinterpret_cast<uintptr_t>(static_cast<const VersionedTupleStorage *>(copy)->data));
            }
        }
        table._readers.leave(slot);
        writer.join();
        EXPECT_EQ(table._inPlaceWrites, nullptr);
        for (tid_t tid = 0; tid < tuples; ++tid) {
            const VersionEntry * entry = table._version_mgmt_column[tid].get();
            const bool wasWritten = tid < written;
            EXPECT_EQ(Native::Sql::toString(*get_current_master(tid, table)), "(" + std::to_string(tid) + ", " + (wasWritten ? "7" : "0") + ")");
            EXPECT_EQ(chain_head(entry), entry);
            EXPECT_EQ(table._head_ident[tid], entry->ident);
            const void * version = reader.visible(entry);
            ASSERT_EQ(version != entry, wasWritten);
            EXPECT_EQ(Native::Sql::toString(*load_version(tid, table, version)), "(" + std::to_string(tid) + ", 0)");
            EXPECT_EQ(table._versionMaps.at(later)[tid], table._versionMaps.at(mapped)[tid]);
        }
    }

    // main and two branches insert beside scans, aggregates and a writer of other branches; the inserts
    // grow the storage several times, which waits for the others
    TEST(ConcurrencyTest, InsertsBesideReaders) {
        constexpr int64_t tuples = 1000, groups = 4, mainInserts = 1500, perGroup = tuples / groups;
        const size_t threads = Parallel::threads, morselSize = Parallel::morselSize, mapAfter = version_map_after_scans;
        Parallel::threads = 4;
        Parallel::morselSize = 256;
        constexpr size_t mapAfterScans = 8;
        version_map_after_scans = mapAfterScans; // the readers' maps are built while others insert

        Database db;
        exec(db, "create table t ( k integer not null , g integer not null , x integer not null );");
        Table & table = *db.getTable("t");
        {
            QueryContext ctx(db);
            ctx.executionContext.branchId = master_branch_id;
            db.constructBranchLineage(master_branch_id, ctx.executionContext);
            for (int64_t k = 0; k < tuples; ++k) {
                std::vector<std::unique_ptr<Native::Sql::Value>> values;
                values.push_back(Native::Sql::Value::castString(std::to_string(k), table.getCI("k")->type));
                values.push_back(Native::Sql::Value::castString(std::to_string(k % groups), table.getCI("g")->type));
                values.push_back(Native::Sql::Value::castString("0", table.getCI("x")->type));
                Native::Sql::SqlTuple tuple(std::move(values));
                insert_tuple(tuple, table, ctx);
            }
        }
        for (auto name : {"s0", "s1", "i0", "i1", "w0"}) exec(db, std::string("create branch ") + name + " from main;");
        const size_t initialCapacity = table.capacity();

        std::mutex failuresLatch;
        std::vector<std::string> failures;
        auto fail = [&](const std::string & message) {
            std::lock_guard<std::mutex> lock(failuresLatch);
            failures.push_back(message);
        };
        auto check = [&](const std::string & who, int64_t got, int64_t expected) {
            if (got != expected) fail(who + ": sum " + std::to_string(got) + ", expected " + std::to_string(expected));
            return got == expected;
        };
        auto insert = [&](const std::string & branch, int64_t k, int64_t x) {
            exec(db, "insert into t version " + branch + " ( k , g , x ) values ( " + std::to_string(k) + " , " +
                     std::to_string(k % groups) + " , " + std::to_string(x) + " );");
        };

        std::atomic<bool> done(false);
        std::atomic<int64_t> branchInserts[2] = {{0}, {0}};
        std::vector<std::thread> pool;
        pool.emplace_back([&] {
            try {
                for (int64_t r = 0; r < mainInserts; ++r) {
                    insert("main", tuples + r, 1);
                    if (r % 100 == 99 && !check("main", sumOf(db, "main"), r + 1)) break;
                    if (r % 500 == 250) {
                        // a fork sees main's inserts so far, and inserts of its own
                        exec(db, "create branch fm from main;");
                        insert("fm", 1000000 + r, 1);
                        check("fork of main", sumOf(db, "fm"), r + 2);
                        exec(db, "drop branch fm;");
                    }
                }
            } catch (const std::exception & e) {
                fail(std::string("main: ") + e.what());
            }
            done = true;
        });
        for (int w = 0; w < 2; ++w) {
            pool.emplace_back([&, w] {
                const std::string branch = "i" + std::to_string(w);
                try {
                    for (int64_t r = 0; !done; ++r) {
                        insert(branch, 100000 * (w + 1) + r, 1);
                        branchInserts[w] = r + 1;
                        if (r % 25 == 24 && !check(branch, sumOf(db, branch), r + 1)) return;
                    }
                } catch (const std::exception & e) {
                    fail(branch + ": " + e.what());
                }
            });
        }
        pool.emplace_back([&] {
            try {
                for (int64_t r = 0; !done; ++r) {
                    exec(db, "update t version w0 set x = x + 1 where g = " + std::to_string(r % groups) + ";");
                    if (!check("w0", sumOf(db, "w0"), perGroup * (r + 1))) return;
                }
            } catch (const std::exception & e) {
                fail(std::string("w0: ") + e.what());
            }
        });
        // readers of unchanged branches never see the inserted tuples: scans of s0, which get a version
        // map, and aggregates of s1 and of both
        std::atomic<size_t> scans(0);
        scannedRows = 0;
        changedRows = 0;
        for (int reader = 0; reader < 2; ++reader) {
            pool.emplace_back([&] {
                try {
                    while (!done) {
                        QueryCompiler::compileAndExecute("select x from t version s0;", db, (void *) &countRow);
                        ++scans;
                    }
                } catch (const std::exception & e) {
                    fail(std::string("s0: ") + e.what());
                }
            });
        }
        pool.emplace_back([&] {
            try {
                while (!done) {
                    if (!check("s1", sumOf(db, "s1"), 0)) return;
                }
            } catch (const std::exception & e) {
                fail(std::string("s1: ") + e.what());
            }
        });
        pool.emplace_back([&] {
            try {
                while (!done) {
                    rows.clear();
                    QueryCompiler::compileAndExecute("select branch, sum(x) from t version ( s0 , s1 ) group by branch;", db, (void *) &collect);
                    if (rows.size() != 2 || std::count(rows.begin(), rows.end(), "(s0, 0)") + std::count(rows.begin(), rows.end(), "(s1, 0)") != 2) {
                        fail("aggregate: " + std::to_string(rows.size()) + " rows" + (rows.empty() ? "" : ", first " + rows[0]));
                        return;
                    }
                }
            } catch (const std::exception & e) {
                fail(std::string("aggregate: ") + e.what());
            }
        });
        for (auto & thread : pool) thread.join();
        Parallel::threads = threads;
        Parallel::morselSize = morselSize;
        version_map_after_scans = mapAfter;

        for (auto & message : failures) ADD_FAILURE() << message;
        EXPECT_GT(scans, mapAfterScans);
        EXPECT_EQ(scannedRows, scans * tuples);
        EXPECT_EQ(changedRows, 0u);
        EXPECT_EQ(table._versionMaps.count(db._branchMapping.at("s0")), 1u);
        EXPECT_EQ(sumOf(db, "main"), mainInserts);
        EXPECT_EQ(sumOf(db, "s0"), 0);
        EXPECT_EQ(sumOf(db, "i0"), branchInserts[0]);
        EXPECT_EQ(sumOf(db, "i1"), branchInserts[1]);
        // the storage grew at least twice, and every array has an element per tuple
        const size_t size = table.size();
        EXPECT_EQ(size_t(tuples + mainInserts + 3 + branchInserts[0] + branchInserts[1]), size);
        EXPECT_GE(table.capacity(), 4 * initialCapacity);
        EXPECT_EQ(table._version_mgmt_column.size(), size);
        EXPECT_EQ(table._head_ident.size(), size);
        EXPECT_EQ(table.getMembership().size(), size);
        for (auto & [branch, versions] : table._versionMaps) EXPECT_EQ(versions.size(), size) << "branch " << branch;
    }

    // an insert that fits into the storage runs beside a statement that reads another branch; one that
    // makes the storage grow waits for it
    TEST(ConcurrencyTest, InsertsWaitOnlyToGrow) {
        constexpr int tuples = 1000;
        Database db;
        exec(db, "create table t ( k integer not null , x integer not null );");
        Table & table = *db.getTable("t");
        {
            QueryContext ctx(db);
            ctx.executionContext.branchId = master_branch_id;
            db.constructBranchLineage(master_branch_id, ctx.executionContext);
            for (int k = 0; k < tuples; ++k) {
                std::vector<std::unique_ptr<Native::Sql::Value>> values;
                values.push_back(Native::Sql::Value::castString(std::to_string(k), table.getCI("k")->type));
                values.push_back(Native::Sql::Value::castString("0", table.getCI("x")->type));
                Native::Sql::SqlTuple tuple(std::move(values));
                insert_tuple(tuple, table, ctx);
            }
        }
        exec(db, "create branch r from main;");
        exec(db, "create branch i from main;");
        const size_t capacity = table.capacity();
        ASSERT_GT(capacity, size_t(tuples));

        // a statement that reads r holds the table and r shared
        std::shared_lock<SharedLatch> tableLatch(table._latch);
        std::shared_lock<SharedLatch> branchLatch(db._branches.at(db._branchMapping.at("r"))->latch);
        auto insert = [&](size_t k) {
            return std::async(std::launch::async, [&db, k] {
                exec(db, "insert into t version i ( k , x ) values ( " + std::to_string(k) + " , 1 );");
            });
        };
        for (size_t k = tuples; k < capacity; ++k) {
            auto inserted = insert(k);
            if (inserted.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
                ADD_FAILURE() << "insert " << k << " waits for the reader";
                tableLatch.unlock();
                branchLatch.unlock();
                return;
            }
            inserted.get();
        }
        EXPECT_EQ(table.size(), capacity);
        EXPECT_EQ(table.capacity(), capacity);
        // the next tuple does not fit: the storage, which the reader may use, has to grow
        auto grows = insert(capacity);
        EXPECT_EQ(grows.wait_for(std::chrono::milliseconds(200)), std::future_status::timeout);
        EXPECT_EQ(table.size(), capacity);
        tableLatch.unlock();
        branchLatch.unlock();
        grows.get();
        EXPECT_EQ(table.size(), capacity + 1);
        EXPECT_GT(table.capacity(), capacity + 1);
        EXPECT_EQ(sumOf(db, "i"), int64_t(capacity + 1 - tuples));
        EXPECT_EQ(sumOf(db, "r"), 0);
        EXPECT_EQ(sumOf(db, "main"), 0);
    }

    // a version map walked before another branch inserts, and added afterwards, has an entry for the new tuple
    TEST(ConcurrencyTest, VersionMapsCoverInsertsDuringTheirWalk) {
        constexpr int tuples = 100;
        Database db;
        exec(db, "create table t ( k integer not null , x integer not null );");
        for (int k = 0; k < tuples; ++k) exec(db, "insert into t ( k , x ) values ( " + std::to_string(k) + " , 0 );");
        exec(db, "create branch r from main;");
        exec(db, "create branch i from main;");
        Table & table = *db.getTable("t");
        const branch_id_t reader = db._branchMapping.at("r");

        auto walked = compute_version_map(table, reader);
        ASSERT_EQ(walked.size(), size_t(tuples));
        exec(db, "insert into t version i ( k , x ) values ( 100 , 1 );");
        add_version_map(table, reader, std::move(walked));
        auto & versions = table._versionMaps.at(reader);
        ASSERT_EQ(versions.size(), table.size());
        EXPECT_EQ(versions[tuples], 1u); // r sees no version of it
        // later inserts extend the map
        exec(db, "insert into t version i ( k , x ) values ( 101 , 1 );");
        ASSERT_EQ(versions.size(), table.size());
        EXPECT_EQ(versions[tuples + 1], 1u);

        scannedRows = 0;
        changedRows = 0;
        QueryCompiler::compileAndExecute("select x from t version r;", db, (void *) &countRow);
        EXPECT_EQ(scannedRows, size_t(tuples));
        EXPECT_EQ(changedRows, 0u);
        EXPECT_EQ(sumOf(db, "i"), 2);
    }

}
