# Development notes

What changed in TardisDB for each experiment, how it was checked and what it measured.
The `README.md` at the root of the repository describes how to reproduce the numbers of the paper
(`results/paper/`); the other measurements are in `results/<experiment>/`, one directory per experiment
(`branchBench --mode=...`), and `experiments.sh` reproduces them. Where an experiment was measured again after a change, the newer
measurement replaced the older one.

# Setup

**Machine**: Ubuntu 24.04.5 LTS (Linux 7.0.0-38), Intel Xeon W-2295 (3.00 GHz, 4.60 GHz turbo, 18 cores /
36 hardware threads, 24.75 MiB L3; CPUs 0-17 are the physical cores, 18-35 their hyperthreads),
128 GiB DDR4 ECC (2 x 64 GiB Micron 36ASF8G72PZ-3G2E1, configured at 2933 MT/s), 512 GB NVMe SSD
(SK hynix PC801).

**Builds and file names** in `results/<experiment>/`:

| prefix | build | threads |
|---|---|---|
| `t1_asis` | TardisDB as is (commit `0d805c4`, `asis/tardisdb.patch`), LLVM 7 and GCC 9.4 in the container of `Dockerfile` (Ubuntu 20.04) | one core |
| `t1_lineage`, `t1_interval` | this branch with TardisDB's lineage map or with interval labels (`-DINTERVAL_VISIBILITY=ON`), LLVM 7 in the container | one core |
| `t1_visbench` | `visbench.cpp`, standalone, GCC 13.3 (`-O3 -march=native`) | one core |
| `eager`, `lazy` | interval labels with eager or lazy membership (`-DLAZY_MEMBERSHIP=ON`), LLVM 18, GCC 13.3 | 18 threads on cores 0-17 |
| `lazy_sets` | the first lazy membership (commit `7bd466d`, sets per branch), LLVM 18 | 18 threads on cores 0-17 |

The `t1_` measurements ran pinned to one core (core 8; `visbench` core 9), before queries ran in
parallel. The paper's measurements (`results/paper/`) use LLVM 18 natively with 18 threads (see
"The paper's results with LLVM 18"); the sections below that quote `results/paper/` before that
measured LLVM 7 in the container.

**TardisDB as is** (`asis/run.sh [TardisDB checkout at 0d805c4] [output directory]`; without a checkout,
it exports `0d805c4` from this repository). `asis/tardisdb.patch` changes:

* `BRANCH_BITMAP_COLUMNS`: TardisDB preallocates the row-major branch bitmap for 8 branches
  and `BitmapTable::resize()` throws `NotImplementedException`, so a table supports at most 8 branches.
  The benchmarks preallocate 256 columns (32 bytes per tuple). Default behaviour is unchanged.
* `HOIST_LINEAGE_COPY` (second build only): `get_latest_entry` copies the lineage map of the branch
  for every tuple (`version_management.cpp`); the variant copies it only when the scanned branch changes.
* A CMake target for `branchBench` (`asis/branchBench.cpp`).

**Port to current LLVM**: the same source builds against LLVM 7 (container) and against LLVM 18 natively
on Ubuntu 24.04 (GCC 13.3, Boost 1.83):

```
cmake -DCMAKE_BUILD_TYPE=Release -DINTERVAL_VISIBILITY=ON -DTARDISDB_LLVM_VERSION=18 <TardisDB>
make -j branchBench tester
```

* `codegen/LLVMCompat.hpp`: `llvm::TypeBuilder` for LLVM >= 9 (removed upstream; integers by size,
  pointers, function types incl. varargs), `getOrInsertFunction` (returns `FunctionCallee` since
  LLVM 9), `getInt8PtrTy`/`getIntNPtrTy` (removed in LLVM 18), `createMemCpy`/`createMemSet`
  (`MaybeAlign` since LLVM 10), atomic loads (`makeMonotonic`) and the code generator's optimisation
  level (`CodeGenOptLevel` since LLVM 18).
* Opaque pointers (LLVM >= 15): every load and GEP states its element type; SQL values take it from
  their SQL type and the join list from its header type instead of deriving it from the pointer.
  Pointer comparisons compare pointers instead of computing their difference.
* LLVM 16 made the block and instruction lists private: `BasicBlock::insertInto`, `BasicBlock::front`.
* The legacy pass manager of LLVM 18 has no aggressive DCE; plain DCE is used there.
* GCC 13: missing standard includes; `std::make_unique`.
* CMake: `TARDISDB_LLVM_VERSION` (default 7) selects the LLVM major version.

**Multi-threaded execution**: TardisDB compiled every query into one sequential loop. Queries now run
morsel-driven (`foundations/Parallel.*`):

* **Scheduler**: a persistent pool of `--threads` workers (default: all hardware threads; 1 keeps
  the serial code paths) takes morsels of 16 k tuples (`--morsel`) from an atomic counter.
* **Scans**: if only selections lie between a `TableScan` and the result, the scan generates a
  function `table_scan_morsel(begin, end, ...)` holding the scan loop and the operators above it,
  and the query function hands it to the workers. Plans with joins, group-by, updates, deletes or
  inserts keep the serial code. `FunctionGen` may now be nested.
* **Results**: `TupleStream` and `Print` take a lock around the callback (or `printf`) of a worker
  and count tuples in memory owned by the execution context instead of a stack slot of the query
  function.
* **Chain walk**: workers cannot call `get_latest_entry`, which writes the reader into the shared
  context; `get_latest_entry_of_reader` walks the chain with the reader's label (or lineage map),
  and `get_visible_ranges` keeps its scratch per thread.

Scaling over 1 to 36 threads (cores 0-35) is in `results/paper/scaling_*.csv`. TardisDB as is cannot
run in parallel and stays single-threaded.

**Invalid IR from the original code generator**: with `TARDISDB_VERIFY_IR=1`, the executor verifies the
generated module before compiling it. The generated code of all tests except the code generator's own
unit tests passes the LLVM verifier, with LLVM 18 and LLVM 7. Fixed:

* Calls declared with other signatures than the C++ functions: `insert_tuple_with_binding` (three
  instead of four parameters, no result), `delete_tuple_with_branchId` (three instead of four),
  `update_tuple_with_binding` (a pointer instead of the branch), `get_latest_entry` in the serial scan
  (a pointer instead of the branch).
* The overflow intrinsics were declared with two overloaded types (`llvm.sadd.with.overflow.i32.i32`).
* The overflow flag, a C++ `bool`, was stored as `i1` through an `i64*` and loaded as 64 bits.
* Division by zero emitted `ret void` in the middle of a block, also in functions that return a value.
* The membership bit test shifted an `i8` by an `i32`.
* LLVM 7 (typed pointers): SQL values and the tuple counter were stored through `i8*`.

The unit tests of the code generator (`TypesTest`, `IfTest`, `LoopTest`, `PhiTest`, `CodegenTest`,
`ExpressionTest`, `JoinTest`) still build invalid functions, mostly functions declared `void` whose
result the tests read; `ExpressionTest.OverflowTest` now declares its result.

**Checks**: `branchBench --mode=verify [--deep]` compares every branch with a reference model after random
forks, updates, inserts and deletes, in which a fork copies its parent's state, and also compares
aggregates over sets of branches (`VERSION ALL`, a random set with and without a predicate,
`ALL DESCENDANTS OF` / `ALL LEAVES OF` a random branch, a single branch) with both strategies of the
cross-branch aggregation; `--schema` adds schema changes (see branch-local schema changes).
Currently, the test suite (`tester`, 86 tests, excluding `InteropTest.*`, which crashes on TardisDB as
is) passes in the three LLVM 18 builds (eager, lazy, lazy with the lineage map) and in both LLVM 7
builds (container; eager and lazy); `verify` passes in the three LLVM 18 builds, with and without
`--schema`, serially (3 000 operations), with `--threads=8 --morsel=64` and with `--threads=36
--morsel=32 --deep` (1 000 operations each). The sections below name the tests that check each change.

# Fork and membership (`--mode=fork`, `results/fork/`)

Latency of `Database::createBranch` from `master` over the table size, median of 7.

* **More than 8 branches**: `BitmapTable::resize()` doubles the bytes per row and copies the rows.
  Fixed on the way: the `Vector(elementSize, reserveCount)` constructor sized its buffer from the
  default capacity instead of `reserveCount` and wrote past it.
* **Membership** (Section V-B): CMake option `-DLAZY_MEMBERSHIP=ON` (default `OFF`, the eager bitmaps
  measured in the paper); `BranchMembership` in `foundations/Database.hpp` holds both implementations.
  * *Eager*: one bitmap column per branch; a fork copies the column of its parent.
  * *Lazy, sets per branch* (`lazy_sets`, commit `7bd466d`, as first described in V-B): a fork only
    records the parent; a branch reads the membership of its parent until it deletes a tuple itself;
    its first change creates a sparse set of its own entries, which becomes a bitmap once it holds more
    than one entry per 256 tuples; before a branch changes the membership of a tuple, it hands its old
    value to every child that reads the tuple from it. Two costs remained: a tuple deleted in some
    branch makes readers walk the entries of their ancestors (depth-dependent), and a delete in a
    branch with many children copies the old value into each of them (bisect: 256 snapshots of `main`).
  * *Lazy, entries per identifier* (`lazy`, current): a change of membership (a delete, or an insert by
    a merge) is an entry (identifier, value) of the tuple, under the identifier the branch writes with.
    A fork gives the parent and the child new identifiers, so later changes of the parent do not
    reach the child, as for versions, and nothing is copied into children. A reader sees the entries
    of the ancestors of its identifier; the one with the deepest identifier, i.e. the containing label
    with the largest left end, decides, independent of the depth. `DROP BRANCH` removes the entries of
    the identifiers of the dropped branches. There is no bitmap per branch any more.
  * Both lazy variants: inserts need no entry, since the versions decide who sees an inserted tuple, so a
    branch *contains* the tuples others inserted, and scans skip members without a visible version
    (`invisible_version`). One byte per tuple holds the membership of the root and of the other
    branches without entries, and whether the tuple has entries; the compiled scan tests it inline.
    The lazy variants keep this byte per tuple (16 MiB at 10 M tuples is the capacity of the vector).

Checks: `test/membership_test.cpp` compares every branch with a copy-on-fork model under bursts of
deletes.

| | eager | lazy, sets per branch | lazy, entries per identifier |
|---|---|---|---|
| fork, 1 M / 10 M tuples | 6.5 ms / 42 ms | 0.45 / 0.30 µs | 1.0 / 0.28 µs |

The effect of membership on the other experiments: agent loops and the memory they leave
(`results/agentic/`), the scan at depth 256 (`results/depth/`) and the shared aggregation over 1 024
branches (`results/crossbranch/`), each in its section.

# Scans over the depth of the branch tree (`--mode=depth`, `results/depth/`)

1 M tuples `t(a INTEGER, b INTEGER)`, a chain of branches `master -> b1 -> ... -> bD`. After each fork,
the new branch and `master` each update `frac` of random tuples. The scan
`SELECT a FROM t VERSION <leaf> WHERE a = 2147483000` matches no tuple, so it measures the bitmap test,
the chain walk and the attribute load but no output. Median of 5 runs after one warm-up.

**`visbench`** (standalone, `t1_visbench_*`): the same workload outside the engine; a reader in the leaf
walks each chain to the first visible version, once with TardisDB's lineage-map test and once with
interval labels on branch identifiers (a fork gives the parent and the child a new identifier). Both
tests are checked to select the same version for every tuple. Median of 5 runs.

Changes in TardisDB (commits `53c4403` and `2f649fa`):

* **Lineage copy**: `get_latest_entry` copies the lineage map only when the scanned branch changes
  (`t1_asis_hoist_*` measures the same change on TardisDB as is).
* **Interval visibility** (CMake `-DINTERVAL_VISIBILITY=ON`): a fork gives the parent and the child a
  new identifier (`Database::createBranch`); identifiers carry interval labels with gaps
  (`Database::newIdent`: a child takes half of the labels its parent has left; `Database::relabel`
  reassigns all labels in metadata when a gap runs out). Versions store the identifier (`ident`).
  A version is visible iff its identifier's label contains the reader's label.
* **Compiled fast path**: `_head_ident` holds per tuple the identifier of the in-place version if it
  heads the chain; the generated scan reads it and calls `get_latest_entry` only otherwise.
* **Version maps** (chain length, Section V-A): a branch can keep a version map
  (`Table::_versionMaps`; `build_version_map()`): per tuple, 8 bytes that say which version the branch
  reads, in place, none, or a pointer to the data of a version in the chain. No tuple data is copied;
  the scans of the branch load the map entry instead of testing the head of the chain and walking it,
  and then load the attributes from where the version lies. What a branch reads changes only by its
  own writes, because every other branch writes under an identifier it does not see; so the map needs
  no invalidation, only maintenance: the branch's own writes set its entries, an insert appends an
  entry (visible only to the inserting branch), and an in-place write of `main` that keeps a copy of
  the old version points the branches that read the old version in place to the copy (also when a
  merge into `main` moves a version inserted by another branch). `DROP BRANCH` removes the map;
  garbage collection keeps every version a map points to. A scan builds the map of its branch once
  `version_map_after_scans` scans of it were compiled (`branchBench --version_map=k`; 0, the default,
  never).

Checks: `verify` passes for 3 random and 2 deep seeds (about 300 branches each, up to 29 relabels) in
the lineage and the interval build, and with `--version_map=1` and `3`; `VersionMapTest` fails without
the update for in-place writes.

`eager_*`, `lazy_*`, `lazy_sets_*`: 18 threads; `*_vmap_*`: the version map of the leaf built before its
scans. 1 M tuples, lazy build, the scan by chain walk (`lazy_frac*`) in parentheses:

| depth | leaf, 1 % per level, version map (chain walk) | leaf, no updates, version map (chain walk) | building the map, 1 % |
|---|---|---|---|
| 1 | 1.22 ms (1.64) | 1.18 ms (1.38) | 18 ms |
| 16 | 1.95 ms (5.26) | 0.95 ms (1.31) | 31 ms |
| 256 | 2.86 ms (15.21) | 0.99 ms (1.22) | 53 ms |

(eager: 3.7 ms at depth 256). The map costs 8 bytes per tuple and one serial pass over the chains; at
depth 256 it pays off after about four scans. When to build it, i.e. how often a branch is read
compared with how much it diverges, is the open question of Section V-A. Membership hardly matters
for the chain walk at depth 256 with 1 % updates: 14.6 ms eager, 14.0 ms with sets per branch, 15.2 ms
with entries per identifier.

# Agent loops (`--mode=agentic`, `results/agentic/`)

`--topology=mcts|beam|bisect` runs agent loops (see `AGENTIC_BENCHMARK.md`); `membership_mib` reports
the memory of the membership structures.

**Loops in SQL** (loops figure of the paper, `results/paper/loops*.csv`, `bench.sh loops`): every phase is
a statement: `CREATE BRANCH`, `UPDATE t VERSION x SET b = v WHERE g = k` on `t(a, b, g)` (g: random
groups of `--mutations` tuples), `SELECT sum(b)` (beam: one `VERSION (x1, ..., x8)` query per round),
`DROP BRANCH` and `MERGE BRANCH x INTO main`. Each sum and `main` after the merge are checked against a
model (a branch = the value of b per group); removing the model update of `MERGE` makes beam fail.
beam forks a workspace that adds the column `score` (branch-local, default 0), which its candidates
write beside `b`; the merge takes it over into `main`, and the sum of `score` in `main` is checked too.
`--trace` writes the statements. The phases are timed without compilation (`compile_ms`: wall time
minus execution time of the statements with a plan; branch statements count as execution).
Medians of 3 runs, LLVM 7, 18 threads:

* Updates are the largest phase of mcts and beam (mcts 27.8 / 27.5 s of 43.8 / 44.1 s with the lineage
  map / labels): `UPDATE` pipelines run on one thread (`TableScan::isParallelPipeline` admits only
  selections below a result).
* beam: labels cut the evaluations from 1.75 to 0.12 s (the lineage build computes the cross-branch
  query as one scan per branch, `BranchAggregation.cpp`), and the updates from 9.6 to 5.1 s (chains up
  to depth 51); its `ALTER TABLE` takes 4 to 6 ms.
* bisect: 15.8 / 15.3 s of 20.9 / 20.6 s are the drops of the 256 snapshots, which examine every tuple `main` changed
  since their fork.
* Compilation: 0.5 to 2.0 s per loop. mcts costs the same with both visibility tests (earlier runs:
  47.2 / 42.1 and 48.7 / 44.7 s; the difference is noise).

The tables below are from the earlier loops, which mutated through the C++ API and neither dropped nor
merged (`--churn=c` added c deletes and c inserts to every mutation); the paper's first version used
`results/agentic/paper_v1_cxx_*.csv`.

`t1_lineage.csv`, `t1_interval.csv`: one core, 1 M tuples. `t1_mcts_fastpath_ab.txt` compares the fast
path reading `_head_ident` with an earlier variant that read the version entry: the latter is faster
for MCTS, where 39 % of the tuples carry branch-private versions in front of the in-place version.

Membership, 18 threads, 1 M tuples (`eager_*`, `lazy_sets_*`, `lazy_*`):

| | eager | lazy, sets per branch | lazy, entries per identifier |
|---|---|---|---|
| mcts / beam / bisect, total | 10.3 / 7.5 / 2.48 s | 5.0 / 4.2 / 0.44 s | 5.4 / 4.3 / 0.60 s |
| membership after the loops | 61 MiB | 1.1 MiB | 1.0 MiB |
| with churn 100: total | 10.7 / 8.6 / 2.06 s | 6.1 / 6.1 / 2.05 s | 5.7 / 4.2 / 0.46 s |
| with churn 100: beam evaluation, bisect mutation | 9.0 ms, 0.36 s | 13.4 ms, 1.97 s | 8.9 ms, 0.39 s |
| with churn 100: membership after bisect | 62.6 MiB | 38.3 MiB | 2.5 MiB |

# Cross-branch aggregation (`--mode=crossbranch`, `results/crossbranch/`)

1 M tuples, k branches forked from `master`, each updating `frac` of random tuples. Per k: k
single-branch queries (sum of medians of 3 runs), one query with k scans and one query with the shared
scan (medians of 5 runs after a warm-up); all three must return the same sums. Baselines run up to
k = 256 (`--maxk_baseline`).

* **SQL** (the query of the former Listing 1 of the paper): `SELECT branch, count(*), sum(b), min(b), max(b), avg(b) FROM t
  VERSION <set> [WHERE c = v] GROUP BY branch`, where `<set>` is `( b1 , b2 , ... )`, `ALL`,
  `ALL DESCENDANTS OF x` or `ALL LEAVES OF x` (parser: `tools/sqlParser/SQLParser.cpp`; analyser:
  `SelectAnalyser::verifyBranchQuery/constructBranchQuery`). Aggregates without `GROUP BY` on a single
  branch work as well. Limits: one table, only `GROUP BY branch`, equality predicates (as the existing
  parser; since then also `IS [NOT] NULL`, see NULL values). Counts and integer sums are returned as `NUMERIC(18,0)`, since
  results cannot carry 64-bit integers.
* **Operator** `BranchAggregation` (logical and physical, `algebra/physical/BranchAggregation.cpp`),
  two strategies (`Logical::BranchAggregation::_strategy`):
  * *PerBranch*: one scan per branch in one query, with the same visibility test as `TableScan`.
  * *Shared* (interval build only): readers are ordered by label. A tuple whose in-place version
    heads its chain, lies in the bitmaps of all readers (word-wise mask test on the bitmap row) and
    whose label contains the smallest and the largest reader label is aggregated once. Otherwise
    `get_visible_ranges()` (`foundations/version_management.cpp`) walks the chain once and returns
    ranges of readers that see the same version; count and sum go to a difference array over the
    readers (two updates per range), min and max update every reader of the range.
* **In parallel**: `BranchAggregation` generates `branch_aggregation_morsel(begin, end, worker,
  position)`; every worker aggregates into its own groups, which lie at least a cache line apart
  (without this, 7 threads were no faster than one), and `finish_state` merges them. In the PerBranch
  strategy, all morsels of one branch are handed out before those of the next.
* Result order now follows the projection (`TupleStream`); before, `SELECT a, b` returned `(b, a)`.

Checks: `verify` compares the aggregates with the reference model after every 500 operations and at
the end (see Setup); it passed for 3 random seeds and 1 deep seed (up to 414 branches, 41 relabels) in
the interval build and for 1 seed in the lineage build.

`t1_interval_frac0.01.csv`, `t1_interval_frac0.001.csv`, `t1_interval_minmax_frac0.01.csv`
(`count(*), sum(b), min(b), max(b)`): one core. 18 threads (`eager_*`, `lazy_sets_*`, `lazy_*`): the
shared aggregation over 1 024 branches takes 208 ms eager, 200 ms with sets per branch and 193 ms
with entries per identifier.

# LLVM 7 and LLVM 18 (`results/llvm/`)

The same queries with the build of LLVM 7 (container) and of LLVM 18 (native), 1 and 18 threads
(`llvm7_t1.csv` ... `llvm18_t18.csv`). Performance is the same within run-to-run variation: e.g.
depth-128 scan 11.1 vs 10.7 ms, shared aggregation over 256 branches 47.9 vs 53.0 ms with 18 threads.

# Branch statements, DIFF and MERGE (`--mode=diffmerge`, `results/diffmerge/`)

TardisDB commit `2f649fa`: the former Listing 1 of the paper runs verbatim in the `sql` shell.

* **`main`** names the root branch as well as `master` (`Database` constructor); results still print `master`.
* **`UPDATE ... SET c = c - 5`**: the right-hand side combines columns and constants with `+ - * /`
  (`*` and `/` bind tighter, no parentheses); the result is cast to the column type (`physical/Update.cpp`).
* **`CREATE INDEX [name] ON t (c) [VERSION b]`**: a hash index local to branch `b` (`BranchIndex` in
  `foundations/Database.hpp`) from key hash to candidate tuples, built at creation and extended by every
  write of `b` (`index_tuple`, also by merges into `b`). A selection `c = constant` on `b` makes the
  `TableScan` loop over the candidates, which are rechecked for visibility and the predicate. One column,
  equality only, not inherited by child branches.
* **`SELECT ... FROM DIFF(t VERSION a, t VERSION b)`**: operator `BranchDiff` (`physical/BranchDiff.cpp`)
  returns, for every tuple whose visible versions differ (or that is visible in one branch only), one row
  per side that has it, with the column `branch`; `SELECT *` yields `branch` and the columns.
* **`MERGE [BRANCH] s INTO t`** (`merge_branch` in `version_management.cpp`): three-way per attribute
  against the version of the last merge of `s` into `t`, else the newest version in the chain visible to
  both. Changes made on one side only are applied, inserts and deletes carried over. An attribute
  changed differently on both sides, or a delete against a change, is a conflict; any conflict aborts the
  merge without changes and lists up to 10 conflicts.
* **`DROP BRANCH b [CASCADE]`** (`drop_branches`): refuses the root, and descendants without `CASCADE`;
  removes `b` and its subtree, clears their bitmap columns, unlinks and frees the versions they wrote,
  drops their indexes and merge bases, and frees the names. Tuples inserted only in dropped branches
  keep their (empty) slot.
* **Over the diverging tuples**: every write records its tuple under the identifier it was written with
  (`Table::_writes`: versions, deletes and inserts, also by merges). Two branches can differ only in
  tuples written under an identifier that is an ancestor of one of them only, i.e. after the fork at
  which their paths split: `diverging_tuples()` collects the writes of these identifiers, and `DIFF` and
  `MERGE` look at these tuples only instead of scanning the table (`diff_scans_whole_table` restores
  the scan for comparisons). `DROP BRANCH` frees the lists of the dropped identifiers.
* On the way: `SELECT *` returns columns in table order (was hash-map order); the `sql` shell links `pthread`.

Limits: `drop`, `cascade`, `merge`, `index` and `on` are now keywords.

Checks: `test/branch_statements_test.cpp`, including a randomised comparison with a reference model that
keeps the full state of every branch (6 seeds x 250 steps of inserts, updates with expressions,
deletes, forks, merges including conflicting ones, drops, index creation and probes, and `DIFF` and
`MERGE` compared with the model after every step).

The experiment: 1 M tuples, a branch forked from `main`, then the branch and `main` each update d random
tuples to the same value (no conflicts); `DIFF` of the two and `MERGE` of the branch into `main`,
medians of 3, 18 threads; the mode fails if both ways return different numbers of rows. Lazy build
(eager is the same within noise):

| d | DIFF / with scan | MERGE / with scan |
|---|---|---|
| 10 | 1.6 / 21.4 ms | 0.07 / 20.9 ms |
| 1 000 | 4.0 / 22.7 ms | 4.3 / 23.5 ms |
| 10 000 | 24 / 41 ms | 46 / 69 ms |
| 100 000 (34 % of the tuples differ) | 219 / 206 ms | 511 / 504 ms |

About 1.5 ms of a `DIFF` is the compilation of the query.

# Branch-local schema changes (`ALTER TABLE ... VERSION b`)

`ALTER TABLE t [VERSION b] ADD [COLUMN] c type [[NOT] NULL] [DEFAULT v]` and `ALTER TABLE t [VERSION b] DROP
[COLUMN] c` change the schema of branch `b` only (default `main`); branches forked from `b` later
inherit it, branches forked before do not see the change.

* **Storage**: an added column is a column of the table like the others (`Table::addBranchColumn`); its
  rows hold the default in place. Which columns a branch has by name is its schema
  (`Table::getSchema()`, the positions of its columns in the order of `SELECT *`): branches without a
  schema change share the columns of `CREATE TABLE`, a fork copies the schema of its parent. Branches
  may add different columns of the same name (two agents trying two definitions of `tier`). `DROP
  COLUMN` removes the column from the schema of the branch only; its storage stays.
* **Versions written before the column**: every version records the number of columns of the table when
  it was written (`VersionedTupleStorage::columns`, right before the tuple data). A version with fewer
  columns reads the default of the others: compiled scans, the cross-branch aggregation and `DIFF`
  test the count only for added columns (`genVersionAttributePtr()`, a load and a compare per version
  read outside the table), and C++ readers pad the tuple (`load_tuple_data()`). A version of a branch
  that is too small is not overwritten in place (`overwrite_own_versions`), but replaced by a new one.
* **Writes keep the columns of other branches**: every version has all columns of the table. The
  logical scan produces all of them, but only the columns of the scanned branch get names
  (`AnalyzingContext::ius`); `UPDATE` reads and writes all (`AnalyzingContext::scanIUs`), so it keeps
  what the version holds for the columns it cannot name, and `INSERT` and `COPY` complete the tuple
  with the defaults (`complete_tuple()`). The in-place writes of `main` thus keep the values another
  branch's added column holds in place.
* **Queries** resolve names in the schema of the scanned branch (`Table::findColumn()`). A query over
  several branches needs the same column in all of them. `DIFF` compares and returns the columns both
  branches have, so two branches that differ only in columns of their own do not differ.
* **`MERGE`** takes over the columns of the source that the target never had, i.e. columns not added
  under an identifier on the target's path: those the source added, or a branch merged into it
  (`ColumnInformation::addedUnder`). The values follow the per-attribute rule, with the default as the
  base. A column added under the same name on both sides is a conflict. Drops are not merged, and a
  column the target dropped is not added back.
* `ALTER TABLE` holds the catalog exclusive, like `CREATE`. Statements are compiled for the schema of
  their branch when they start; there is no plan cache to invalidate.

Limits: a `NOT NULL` column needs a `DEFAULT`, which the existing tuples take (a nullable column without
one is `NULL` in them, see NULL values); `longinteger` and `char` columns cannot be added (no casts from strings), and bool
columns cannot be printed or given as literals (in TardisDB as is). Adding a column writes its default
into every row in place, O(n) under the exclusive catalog latch. The storage of dropped columns and of
columns of dropped branches is not reclaimed. No `RENAME` or change of type. `alter`, `add`, `column`
and `default` are now keywords.

Checks (`test/schema_test.cpp`, 14 tests): columns local to a branch and inherited by later forks;
errors; inserts with and without column lists and updates in the branch beside writes of `main`;
versions written before the column (in place, in the chain, through a version map, with one thread and
with parallel scans); sibling columns of the same name; aggregates over branches with the column, with
both strategies, and `DIFF`; `DROP COLUMN` and a new column under a dropped name; a column of `main`;
a branch-local index on an added column; `MERGE` into the root and along the tree, conflicts on columns
added on both sides, and columns the target dropped; `DROP BRANCH`; and a randomised comparison with
a model in which every tuple has a value for every column of the table (missing: the default), a write
keeps the values of the columns its branch does not have and a fork copies the schema (8 seeds x 300
steps of inserts with random column lists, updates, deletes, forks, `ADD COLUMN` under 4 names, also
those of dropped columns, `DROP COLUMN`, merges including conflicting ones and drops; after every step
`SELECT *` of every branch, with one thread and with parallel scans, and a `DIFF`; 15 to 20 columns
added per seed). `branchBench --mode=verify --schema` adds columns to random branches (6 names, so
that siblings share names), drops them and updates them with SQL beside the random forks, updates,
inserts and deletes, and compares every column of every branch with the reference model.

* Mutations, 1 run each: without the column count test in compiled loads, the tests and `verify` fail;
  without padding in C++ loads, `verify` fails (its updates read the old tuple); overwriting a version
  that is too small fails `VersionsWrittenBeforeTheColumnReadTheDefault` and crashes `verify`; updates
  that write only the columns of their branch fail two tests and `verify`; merge bases that keep their
  old width fail the randomised test; forks that do not inherit the schema fail 7 tests and crash
  `verify`; a merge that does not take over the columns fails three tests. `verify` does not merge, so
  it misses the last two.

Cost (`--mode=schema`, `results/paper/schema.csv`, LLVM 7 in the container, 18 threads, 1M
tuples, `b` forked from `main` updated 1 %, median of 7; Section VI of the paper): `ADD COLUMN` takes
5.8 ms (the default written into 1M rows). The scan that matches no tuple takes 0.67 ms in `main`,
1.41 ms in `b` on `a` and 1.32 ms on the added column, and 1.28 ms after `b` wrote the added column
in its updated tuples (the run checks that the `UPDATE` wrote exactly those). In the `sql` shell
(LLVM 18, all threads) with 1M rows of 3 columns, the sums of `y` and `z1` over `b` match the input.
There, the rows were loaded with 100 `COPY`s of 10,000 rows: one `COPY`
compiles one call per tuple into a single function (`Insert::genInsertCall`), and its time grows
quadratically (0.8 s for 10,000 rows, 3.9 s for 30,000, 33 s for 100,000; the same before this change).
Since then, `Insert` loops over its tuples: 100,000 rows take 0.19 s (see the PostgreSQL server below).

# Garbage collection (`--mode=cycles`, `results/gc/`)

Section V-D, lineage-aware garbage collection:

* **No garbage from repeated writes** (`overwrite_own_versions`): a branch that writes a tuple again
  under the identifier of its earlier version overwrites that version, in place or in the chain,
  since a current identifier has no descendants and no other branch sees it.
* **Collection at `DROP BRANCH`** (`collect_garbage`): after freeing the versions of the dropped
  branches, a version is kept only if a remaining reader needs it: the current identifier of every
  branch, and every identifier with branches in two of its subtrees, whose newest version is the
  base of a merge of branches below it. The readers come from the identifier tree; per tuple, one walk
  of the chain assigns them their versions, as `get_visible_ranges()` does. Only the tuples written
  since the fork (by the dropped branches and by their parent) are examined, so `DROP` costs time in
  proportion to the divergence, not to the table.
* **Write lists**: the list of an identifier whose subtree holds the current identifiers of all
  branches or of none can no longer tell two branches apart and is freed at `DROP`.
* **Identifiers** (added with the transactions of `pgserver`): `DROP` frees the identifiers of the
  dropped branches for reuse by later forks, and folds the parent's identifiers back: the identifier
  it forked the subtree from, left with one child under which the parent continues, absorbs that child
  (`fold_ident()` retags the versions, deletes, membership entries and branch-local columns under the
  child, found in its write list: the parent's writes since the fork). The identifier tree, the paths
  and relabelling thus grow with the forks that are alive, not with all forks so far; `MERGE` and `DIFF`
  walk the paths up to where they split, `DROP` the paths of the dropped branches and of the parent
  since the fork, and the collection the tree from its root (`Database::identPreorder()`). Since
  identifiers are reused, a child's may be smaller than its parent's: ancestry is decided by labels
  (`Database::isAncestorIdent()`), not by order. The in-place version of a tuple that only a dropped
  branch inserted gets an identifier without labels (`Database::deadIdent()`), so that no reader sees
  it under a reused identifier. Branch ids are still not reused.
* `live_versions()` counts the versions in chains; `test/gc_test.cpp` checks that create-and-drop
  cycles leave no versions and that the base of a merge survives a drop (the test fails without the
  identifiers with branches in two subtrees).

The experiment: 1 M tuples, 200 cycles of: fork `c` from `main`, `c` and `main` each update 1 %, scan
`c`, drop `c`; `--keep` adds a branch forked before the cycles; `--gc=false` turns both mechanisms off.
Versions after 200 cycles (lazy build):

| | without collection | with collection |
|---|---|---|
| no long-lived branch | 2 000 000 | 0 |
| with a long-lived branch | 2 000 000 | 864 614 (one per tuple `main` changed, 86.6 %) |

`DROP` takes 11 ms with collection (9 ms without); the scan of `main` stays at 0.6 ms. RSS still grows
slowly (0.6 MiB per 100 cycles on 10 k tuples: compiled queries and identifiers; more at 1 M tuples,
where the allocator keeps freed versions). With eager membership, the bitmap columns of dropped
branches are never reused (30 MiB after 200 cycles).

# Concurrent clients (`--mode=concurrent`, `results/concurrent/`)

Statements may come from several threads at once, e.g. one agent per branch; the work of each
statement is still spread over the workers.

* **Latches** (`tools/queryCompiler/queryCompiler.cpp`, `StatementLatches`): a statement holds them
  from its analysis to the end of its execution, since the compiled code embeds the table size and
  pointers into the version maps. Order: the catalog (`Database::_catalogLatch`), the tables
  (`Table::_latch`) in the order of their addresses, the branches (`Branch::latch`) in the order of
  their ids. `CREATE`, `DROP`, `MERGE`, `COPY` and `CREATE INDEX` hold the catalog exclusive; `SELECT`
  (also `DIFF`), `INSERT`, `UPDATE` and `DELETE` hold it shared. With `QueryCompiler::Latching::Branch`
  (the default; `branchBench --latching=branch`):
  * `SELECT` holds its tables shared and, once the analysis has found them, its branches shared.
  * `UPDATE` and, with eager membership, `DELETE` hold the table shared and their branch exclusive,
    in `main` as in any other branch.
  * `INSERT` holds the table shared and its branch exclusive if the tuple fits into the storage of the
    table without moving it (below), one insert at a time per table (`Table::_insertLatch`, taken before
    the table latch); otherwise it holds the table exclusive and the storage grows.
  * With lazy membership, `DELETE` (an entry in the membership of the table) holds the table exclusive.

  `Latching::Table` (`--latching=table`) latches only the tables, shared for reading and exclusive for
  writing; `Latching::Serial` (`--latching=serial`) runs one statement at a time; both for comparison.
  All latches are `SharedLatch`, a pthread rwlock that prefers writers
  (`PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP`); a thread may hold it shared only once. With
  `std::shared_mutex` of glibc, which prefers readers, `CREATE BRANCH` (catalog exclusive) waited
  indefinitely under overlapping readers while the writers of other branches kept creating versions
  (a test grew to 20 GB).
* **Worker pool** (`foundations/Parallel.cpp`): queries that run at the same time share the threads;
  a free thread joins the job with the fewest workers that still has morsels and stays with it until
  they are gone. Worker numbers are per job, so the per-worker groups of `BranchAggregation` stay
  private.
* **State shared under a shared latch**: compiling a scan counts it and may build the version map of
  the branch; `_scans` and the map `_versionMaps` are guarded by `Table::_scanLatch`, and a version map
  is built before it is added. If two statements build the map of a branch at once, the first one
  stays (`emplace`), since a scan may already use it. The string pool takes a mutex (a deque, so that
  returned references stay valid), the version counter is atomic, and the analysers look branches up
  with `at()` instead of `operator[]`. The write list of an identifier exists from its creation on
  (`newIdent()`, `createTable()`), so a writer only appends to the list of its own identifier;
  `branch_visibility` is no longer maintained by `prepend_version`, since no visibility test reads it
  (only `has_lineage_intersection()`, which has no callers).
* **Publishing a version** of a branch other than `main` (`prepend_version`): a version a branch writes
  carries the branch's current identifier, which no other branch sees, so it changes nothing another
  branch reads. The version is complete before a compare-and-swap makes it the head of the chain, so
  writers of several branches can prepend to the same chain; readers load the head with acquire
  semantics (`chain_head()`) and the `next` fields as well (`chain_next()`), since `main` relinks
  chains while the table is shared (below). The head identifier of the tuple, which compiled scans read
  to skip the chain walk, becomes `invalid_ident` with an atomic store; compiled code loads it
  atomically (monotonic), as it does the bytes of the eager bitmaps, which deletes in other branches
  change with atomic and/or (a row holds the bits of all branches), and the entries of version maps.
* **Inserts**: compiled scans embed pointers into the columns, the null indicators, the membership, the
  head identifiers and the version maps, and the number of tuples, all of which an insert extends. These
  arrays now hold `Table::capacity()` rows without moving (`Table::reserveRows()`; `Vector::reserve()`,
  and `BitmapTable::resize()` keeps the capacity when a fork widens the rows); they grow, doubling, only
  when a statement holds the table exclusive. `Table::size()` counts the published rows (atomic): an
  insert completes the version entry before it appends it, appends the version entry, the head
  identifier and an entry of every version map under the scan latch, stores the values and only then
  publishes the row (release), so a statement compiled meanwhile scans the rows before it. Statements
  of other branches would not see the tuple anyway, since its identifier is not an ancestor of theirs.
  A version map whose walk (`compute_version_map`) overlaps with an insert gets an entry for the new
  tuple when it is added (`add_version_map`, under the scan latch), and the comparison of the head
  identifiers with the version entries (`Table::headsKnown()`) takes the scan latch as well.
* **Writes of `main`** are in place: its version of a tuple is the version entry itself, which every
  branch reads that has not written the tuple. An `UPDATE` in `main` writes in two phases:
  * During the statement (`defer_in_place_write`): the old version is copied into a chain element,
    which takes the place of the version entry in the chain: a compare-and-swap of the head if the
    version entry is the head, otherwise a release store into the `next` field of the element before it
    (other writers only prepend, and only `main` changes what follows the version entry). The head
    identifier becomes `invalid_ident`. The new values go into a buffer of the statement
    (`InPlaceWrites`, reachable through `Table::_inPlaceWrites`), and the entries of other branches'
    version maps that read the tuple in place now point to the copy; a map whose walk overlaps with the
    writes gets the copies when it is added (`compute_version_map`, then `add_version_map` under the
    scan latch). A statement that starts now finds the copy; the data in place still hold the old
    version.
  * At the end of the statement (`finish_in_place_writes`, called from the destructor of the
    statement's latches while `main` is still latched): a grace period (`Table::_readers`,
    `ReaderEpochs`, two counters and a phase that alternates) waits until every statement that holds
    the table shared and began before it has ended; only those may still read an old version in place.
    Then the new values are written in place, the version entry gets `main`'s identifier, points to the
    copy (`next_in_branch`) and becomes the head of the chain again with a compare-and-swap. The head
    identifier is set last and reset to `invalid_ident` if another branch has prepended meanwhile.
  * A statement that holds a table shared enters the table's epoch once it holds all its latches and
    leaves it before its own in-place writes end; the writer of `main` does not enter its own table.
    An eager `DELETE` in `main` changes only `main`'s bit in the bitmaps, which no other branch reads,
    and is not deferred.

Checks (`test/concurrency_test.cpp`):

* `BranchesInParallel` runs six workers that update, insert and delete in their own branches (two of
  them each the same tuples), a writer of `main`, a thread that forks `main` and an unchanged branch,
  writes the fork and drops it, and a thread that aggregates over three branches, all with 4 workers
  per query and morsels of 256 tuples; every thread checks the sum of its branch after every statement,
  and a fork of `main` must see `main` between two of its updates. Without the latches it fails in
  every run (lost updates of `main`, forks that see half an update, "no such tuple").
* `PrependsToSharedChains` has 8 threads update the same 200 tuples, each in its own branch, 40 times,
  with `overwrite_own_versions` off so that every update prepends, while a reader of an unchanged branch
  walks the chains; afterwards every chain has 321 elements and every branch reads its last value. With
  a plain store instead of the compare-and-swap, it fails in 3 of 3 runs (320 elements).
* `MainWritesBesideReaders`: `main` updates groups of 4 000 tuples 40 times and checks its sum, and
  forks and drops a branch every ten updates. Meanwhile, two writers update their branches, two
  threads scan an unchanged branch (every row must read its old value; the branch's version map is
  built after 8 scans, while `main` writes), and two threads aggregate over one and over two branches.
  Afterwards every chain contains its version entry once.
* `InPlaceWritesWaitForReaders` holds a reader's epoch open across an update of `main` and a version
  map added after it. It checks that the in-place writes wait, that the reader meanwhile sees the old
  values through the copies (in the chain and in both maps), and that everything is in place once the
  reader leaves.
* `InsertsBesideReaders`: `main` inserts 1 500 tuples, two branches insert until it is done, and
  each checks its sum; `main` forks a branch three times, which inserts a tuple and is dropped. Meanwhile,
  a writer updates its branch, two threads scan an unchanged branch (with a version map built after 8
  scans) and two aggregate over one and over two unchanged branches; none of them may see an inserted
  tuple. The storage grows at least twice (from 1 024 rows); afterwards every array has an element per
  tuple. `InsertsWaitOnlyToGrow` holds the latches of a reader of another branch: the inserts that fit
  complete meanwhile, the first one that does not waits until the reader has released them.
  `VersionMapsCoverInsertsDuringTheirWalk` walks a version map, inserts in another branch, and adds the
  map afterwards: it has an entry for the new tuple.
* Mutations, 3 runs each, of inserts: if every insert holds the table shared, also when the storage has
  to grow, `InsertsBesideReaders` crashes and `InsertsWaitOnlyToGrow` fails; if every insert holds it
  exclusive, `InsertsWaitOnlyToGrow` fails; without the entry for tuples inserted during the walk of a
  map, `VersionMapsCoverInsertsDuringTheirWalk` fails (the stress test does not notice: the walk takes
  microseconds). Publishing the row before its values are stored goes unnoticed by all tests, since no
  other branch sees the tuple; the order only keeps readers from loading what is not yet written.
* Mutations, 5 runs each, of in-place writes of `main`: without the grace period, without the
  deferral, without patching the maps during the write or when a map is added, and without resetting
  the head identifier, `InPlaceWritesWaitForReaders` fails every time; `MainWritesBesideReaders` catches only the missing
  map patch (5 of 5) and the missing deferral (3 of 5).

The experiment: 1 M tuples `t(a, b, g)`, 18 threads per query on cores 0-17, 5 s per row. An agent
alternates `UPDATE t VERSION c_i SET b = b + 1 WHERE g = r` (1 000 tuples; a serial scan of the table)
and `SELECT b FROM t VERSION c_i WHERE a = x` (a parallel scan). A reader runs the scan only, beside a
sibling branch that is read, written or inserted into (`INSERT INTO t VERSION c_s ( a , b , g ) VALUES
( ... )`, one tuple per statement), or beside a client that updates `main`; 16 agents run beside a
client that updates `main` or inserts into its branch. Eager build,
iterations/s and latency medians, the execution time of the compiled code in parentheses:

| | one at a time | latches per table | per branch | per table, version maps | per branch, version maps |
|---|---|---|---|---|---|
| 1 agent | 34.6 | 41.8 | 37.2 | 52.6 | 53.0 |
| 4 agents | 28.2 | 26.4 | 76.8 | 37.6 | 167 |
| 16 agents | 27.0 | 25.6 | 154 | 38.4 | 520 |
| 16 agents, scan | 5.8 ms (2.6) | 323 ms (18.9) | 25.5 ms (21.7) | 208 ms (5.8) | 7.6 ms (5.0) |
| 16 agents, update | 346 ms (31.1) | 331 ms (32.6) | 75.6 ms (71.1) | 211 ms (18.6) | 21.6 ms (18.6) |
| reader alone | 3.4 ms (1.42) | 3.4 ms (1.42) | 3.5 ms (1.43) | 3.0 ms (1.01) | 2.5 ms (0.98) |
| reader beside a reading sibling | 12.8 ms (1.51) | 5.3 ms (1.49) | 5.4 ms (1.50) | 4.3 ms (1.05) | 4.3 ms (1.05) |
| reader beside a writing sibling | 1018 ms (3.32) | 41.5 ms (2.50) | 6.0 ms (2.94) | 34.2 ms (1.09) | 2.9 ms (1.00) |
| reader beside an inserting sibling | 43.1 ms (1.45) | 9.2 ms (1.51) | 3.4 ms (1.44) | 8.4 ms (1.07) | 2.9 ms (1.01) |
| sibling, inserts/s beside the reader | 632 | 109 | 893 | 119 | 950 |
| reader beside updates of `main` | 162 ms (2.80) | 28.4 ms (3.28) | 6.1 ms (2.76) | 26.3 ms (1.61) | 3.2 ms (1.32) |
| `main`, updates/s beside the reader | 139 | 35.0 | 42.0 | 38.0 | 46.6 |
| 16 agents beside updates of `main` | 24.0 | 17.2 | 151 | 16.0 | 482 |
| `main`, updates/s beside 16 agents | 11.2 | 27.0 | 10.0 | 40.6 | 22.4 |
| 16 agents beside an inserting sibling | 27.0 | 25.6 | 147 | 38.4 | 500 |
| sibling, inserts/s beside 16 agents | 7.0 | 4.6 | 578 | 19.4 | 633 |

* **One at a time**, k agents reach no more than one agent alone, and the mutex is unfair: beside a
  writing sibling, a reader waits 1 s; scans of 16 agents wait up to 1.5 s (p99).
* **Latches per table**: reads of different branches overlap (a reader beside a reading sibling takes
  5.3 instead of 12.8 ms), writes do not: every `UPDATE` holds the table exclusive for its serial scan
  and its compilation, so 16 agents reach no more than one. Since the latches prefer writers, the
  agents' scans wait as long as their updates (323 and 331 ms). Isolation for performance (Section V-B)
  fails: a writing sibling raises a reader's latency from 3.4 to 42 ms, which with version maps
  (3.0 to 34 ms) is waiting for the latch.
* **Latches per branch**: agents in different branches run their statements at the same time: 16
  agents reach 154 iterations/s, and 520 with version maps, ten times one agent. Without version maps,
  every agent walks the versions of all the others: with 16 agents, the execution of a scan grows from
  2.4 to 22 ms and that of an update from 20 to 71 ms (the effect of the cross-branch measurements of
  Section VI, where siblings lengthen the chains all others walk). With version maps, the agents read
  their maps instead; what remains (scans 1.4 to 5.0 ms, updates 14 to 19 ms) is 16 agents sharing 18
  cores.
* Isolation for performance holds with latches per branch and version maps: beside a writing sibling,
  a reader takes 2.9 ms (alone 2.5 ms; in the compiled code 1.00 and 0.98 ms); without version maps
  6.0 ms, of which 1.5 ms are the sibling's versions in the chains. A reading sibling costs more
  (4.3 ms) than a writing one, since its scans
  share the 18 workers while the update runs on one thread.
* **Beside updates of `main`**, a reader no longer waits for them: 3.2 ms instead of 26 ms with version
  maps, and `main` updates more often (47 instead of 38 per second). It still takes 0.7 ms more than
  alone: 0.3 ms in its scan, which reads the tuples `main` has overwritten
  through pointers into the chains (each update of a group moves up to 1 000 versions into the
  chains), and 0.4 ms outside the compiled code, not yet analysed.
* 16 agents keep their throughput beside updates of `main` (482 instead of 520 iterations/s with
  version maps). `main` updates less often than with latches per table (10.0 instead of 27.0 per second
  without version maps, 22.4 instead of 40.6 with them), where the agents reach only 17 and 16
  iterations/s. Probably, its grace period waits for every statement of the agents that is running
  when it ends, and agents' statements take 25 to 76 ms without version maps and 8 to 22 ms with them;
  the writer-preferring latch per table lets `main` go first between statements. This is not measured
  separately; `main`'s rate varies between runs (with latches per table and version maps 14.4 per
  second in the previous run).
* **Beside an inserting sibling**, a reader takes as long as alone with latches per branch (3.4 ms, with
  version maps 2.9 ms), while the sibling inserts 893 tuples/s (950). With latches per table, every
  insert holds the table exclusive: the reader waits (9.2 ms, 8.4 with version maps) and the sibling
  inserts only 109 (119) tuples/s, between the reader's scans. 16 agents beside an inserting sibling
  reach 147 (500) iterations/s, about as many as without it, while the sibling inserts 578 (633)
  tuples/s; with latches per table, 25.6 (38.4) iterations/s and 4.6 (19.4) inserts/s. The table has
  room for 1 048 576 rows after loading 1 M tuples, so no insert of the run grows the storage; an
  insert that does holds the table exclusive, once per doubling of the capacity.
* Lazy build (`lazy_*.csv`): one at a time, 16 agents 27.8 iterations/s; latches per table 22.4 (32.0
  with version maps); per branch 150 (456). With latches per branch and version maps, a reader takes
  3.3 ms alone, 3.4 ms beside a writing sibling, 3.2 ms beside an inserting sibling (9.1 ms with
  latches per table) and 3.9 ms beside updates of `main` (26.8 ms); 16 agents beside updates of `main`
  reach 429 iterations/s (22.4), `main` 21.6 updates/s; beside an inserting sibling 446 (32.0), the
  sibling 670 inserts/s (9.6).

Still exclusive: inserts that grow the storage (each doubling of the capacity moves the arrays that
compiled scans embed), lazy deletes (an entry in the membership of the table) and everything that
changes the catalog. Inserts into the same table also exclude each other (`Table::_insertLatch`),
since they append to the same arrays. Writes would also latch less if compilation ran before the
latches and `UPDATE` ran in parallel.

# PostgreSQL-compatible server (`pgserver`) and the SQL of PostgreSQL clients

`pgserver.cpp` speaks the frontend/backend protocol 3.0, so that `psql`, psycopg2, psycopg 3 (and other
libpq or JDBC clients) connect: `pgserver [-h host] [-p port] [-t threads per query]`, one thread per
connection.

* **Sessions and branches**: a session has a current branch (`SET branch = b`, or `options=-c branch=b`
  when connecting); the parser fills it in for relations without `VERSION` (`ParsingContext::defaultBranch`
  instead of the constant "master"). `SHOW branch`.
* **Protocol**: no SSL (the client is told `N`), no password; the simple query protocol (several
  statements per query string) and the extended one (Parse, Bind, Describe, Execute, Sync, Close;
  parameters in text or binary for bool, int2/4/8, float4/8, numeric, text, date and timestamp; results
  in text). Errors carry an SQLSTATE; after an error in the extended protocol, messages are skipped
  until Sync.
* **Transactions**: statements outside of a block autocommit. A block (`BEGIN` ... `COMMIT`) is a
  transaction with snapshot isolation (`QueryCompiler::Transaction`): `BEGIN` forks a private branch from
  the session's branch, the block's statements read and write it (also under `VERSION` of the session's
  branch), `COMMIT` merges it back with `MergePolicy::FirstCommitterWins` and drops it, `ROLLBACK` drops
  it. A tuple that another transaction, or a statement outside of transactions, wrote since the `BEGIN`
  makes the commit fail with 40001, and nothing of the block is committed; write skew remains possible.
  In a block, only `SELECT`, `INSERT`, `UPDATE` and `DELETE` run, writes only of the session's branch,
  and `SET branch` is refused (25001); `VERSION ALL`, `DESCENDANTS` and `LEAVES` read the committed
  branches as they are. The private branch has no name a statement can use and is not listed; its
  parent cannot be dropped while it exists. A connection that ends in a block rolls it back, and after
  an error of the block, every statement but `COMMIT` and `ROLLBACK` is refused until its end, as in
  PostgreSQL. Keys are not checked at commit: two transactions may insert the same key.
  `test/transaction_test.cpp` (8 tests) checks snapshot reads, own writes, rollback, first-committer-wins
  and which statements run, and has 6 threads increment a counter that they read and write in separate
  statements of a transaction, retrying on 40001, without losing an increment. A commit drops its
  snapshot, which folds main's identifiers back (Garbage collection, Identifiers): over 20 000
  transactions through `pgserver` (1 000 tuples, one thread per query), `COMMIT` stays at 0.13-0.21 ms;
  without folding it grew from 0.6 to 7.8 ms, since merge and drop walked all identifiers so far.
* **Databases**: `tardisdb` exists, others are created with `CREATE DATABASE` and dropped with `DROP
  DATABASE [IF EXISTS]` (not while a session uses them); connecting to another one is an error, as in
  PostgreSQL. Catalog (TardisDB has no `information_schema`): `SHOW DATABASES`, `SHOW TABLES`,
  `SHOW BRANCHES`, `SHOW COLUMNS FROM t` (name, type as PostgreSQL names it, nullable, position in the
  primary key).
* **Engine interface**: `QueryCompiler::execute(query, db, branch, sink, described)` returns the
  columns of the result (names as in the select list, aggregates by their function), the rows returned
  or written (`ExecutionContext::affectedRows`, set by Insert, Update and Delete) and the statement tag.
  The result tuples go to a per-statement sink (`QueryContext::tupleSink`, called through
  `emitToSink(context, tuple)`) instead of the one callback of a module, so that concurrent sessions
  each get their own rows.

The SQL that clients send needed these changes of the parser and the engine (`test/dialect_test.cpp`,
10 tests; `test/test_pgserver.py` with psycopg2 and psycopg 3):

* Tokenizer: a string literal is one token (spaces, commas, parentheses, semicolons, `''`); casts behind
  literals (`'...'::timestamp`, `5::integer`, as psycopg2 writes them) are dropped outside of expressions,
  as TardisDB casts every literal to its column's type (in expressions, see arrays, they are casts);
  `<`, `<=`, `>`, `>=`, `<>`, `!=` are operators, also without spaces; `true` and `false` are constants.
* `WHERE` (one parser for SELECT, UPDATE and DELETE, `parseWhere`): any condition (see arrays); of its
  conjuncts, comparisons of a column and a value (either side), row values compared lexicographically
  (`(a, b) >= (1, 2)`, the range predicate on composite keys), `BETWEEN`, `IN`, `<>` and joins on `=`
  are kept apart. Equalities with a value stay selections (index probes); the others become
  `Predicate`s, which `construct_predicates` turns into comparison expressions (a row value into
  `c0 < v0 OR (c0 = v0 AND ...)`), also over several branches and in `DIFF`.
* Fixed on the way: the logical `ExpressionVisitor` had no `visit` for `Not`, `And` and `Or`, so these
  expressions reached `unreachable()` and could not be used at all; `And` and `Or` left their type
  unset; the logical and the physical `Not` dereferenced their moved-from child; `Select::computeRequired`
  did not look into them. `char` and `varchar` had no `compare` (`<`, `>`; also `=` reaches compare).
* `CREATE TABLE`: `PRIMARY KEY (...)` and `PRIMARY KEY` of a column (recorded in `Table::getPrimaryKey()`,
  for catalogs, not enforced); the type names of PostgreSQL (`smallint`, `int2/4/8`, `bigint`,
  `decimal`, `boolean`, `bpchar`; `float`, `real`, `double [precision]` are double precision, see arrays);
  columns declared without `NOT NULL` are nullable (see NULL values).
* Multi-row `INSERT ... VALUES (...), (...)`. `Insert` loops over its tuples instead of generating one
  call per tuple: `COPY` of 100,000 rows takes 0.19 s instead of 33 s (the generated function grew with
  the number of tuples, and its compile time quadratically).
* Values: `varchar` of a capacity of 256 or more lost its first byte (the native load and store read a
  length of one byte, the generated code two or four) and could corrupt the heap; a string longer than
  its column was written past its slot and is now an error (`value too long`). `char` had no native
  representation (`castString` reached `unreachable()`); it is now stored as the generated code does
  (one byte for `char(1)`, else as a varchar), and result tuples carry it. `longinteger` had no cast
  from strings in the generated code (`unreachable()`, which produced invalid IR) nor a native one, and
  result tuples dropped it. Timestamps take ISO 8601 (`T`), a date alone, and fractions of a second
  (milliseconds; finer digits are cut). The `sql` shell prints `varchar`, `char`, `date`, `timestamp`
  and `longinteger`.
* With `TARDISDB_VERIFY_IR=1`, the generated code of all tests except the code generator's own unit
  tests (Codegen, Expression, If, Loop, Phi, Types) passes the LLVM verifier.

# NULL values

A column declared without `NOT NULL` (or `PRIMARY KEY`) is nullable, in `CREATE TABLE` and in `ALTER
TABLE ... ADD COLUMN`; before, such columns were stored not null and `NULL` was an error.

* **Storage**: a nullable column holds the null indicator beside each value, in place and in every
  version: the layout of the generated code for a nullable type, `{ value, i1 }`
  (`ColumnInformation::NullIndicatorType::Embedded`; the separate `BitmapTable` of null indicators,
  which versions did not have, is no longer used). Versions are tuples of these types, so the chain, the
  version maps, `genVersionAttributePtr` and the merge bases carry the indicator without change. The
  native `NullableValue` loads and stores the same layout, a `NULL` with its value zeroed; columns
  without nulls keep their layout and their code.
* **Statements**: `NULL` among the values of `INSERT`, `SET`, `WHERE` and `DEFAULT` (the parser passes
  it as `Native::Sql::nullLiteral`); a column an `INSERT` leaves out is `NULL` (a column without nulls
  keeps its default, the zeroed value, as before). `WHERE` is three-valued: a comparison with `NULL` is
  `NULL` and holds for no tuple; `c IS [NOT] NULL` (also in queries over several branches and in `DIFF`)
  is never `NULL`. `NULL` or a value that may be `NULL` for a `NOT NULL` column (`SET k = x` with a
  nullable `x`) is refused before execution (`not_null_violation`, SQLSTATE 23502 in pgserver).
* **Aggregates** skip `NULL`s: `count(c)` counts the values, `sum`, `min`, `max` and `avg` of no
  values are `NULL`. `BranchAggregation` gives each aggregate of a nullable column a slot that counts
  its values, which the difference arrays of the shared scan sum like the count of the tuples.
* **MERGE and DIFF** compare versions with `IS NOT DISTINCT FROM` (native `NullableValue::equals`):
  two `NULL`s are equal, a `NULL` and a value are not. Setting an attribute to `NULL` in both branches
  is no conflict.
* `COPY`: an empty field is `NULL` in a nullable column, and `COPY ... TO` writes `NULL` as an empty
  field. `SHOW COLUMNS` reports whether a column is nullable.
* Fixed on the way: `AND` and `OR` of booleans that may be `NULL` generated invalid IR (the condition
  of the `ElseIf` was evaluated in the then-branch); they are selects now. The generated code could not
  load a `bool`; the native `NullableValue` could neither load nor store, `getNull` of the generated
  code had no `date`, `timestamp` and `text` and built a `NULL` boolean of an integer. `COPY ... TO`
  wrote the columns in hash-map order, and the varchars `COPY ... FROM` read pointed into lines freed
  before the insert ran.
* The benchmark drivers add beam's column as `INTEGER NOT NULL DEFAULT 0` (`branchBench.cpp`,
  `compare/compare.py`, which also replays the earlier traces without `NOT NULL`), so that the paper's
  loops keep a column without nulls.

Checks: `test/null_test.cpp` (9 tests: inserts, the conditions, updates, aggregates in one branch and
across branches with both strategies, `MERGE` and `DIFF`, added columns, `COPY`, parallel scans), with
`TARDISDB_VERIFY_IR=1`; `test/dialect_test.cpp` and `test/schema_test.cpp` expect `NULL`s where they
expected errors.

# Joins

Joins (`FROM a, b WHERE a.id = b.id`, also of three tables, on several columns and of two versions of
one table) crashed while their code was generated under LLVM 18. `LoopGen::loopBodyEnd` set the insert
point into the block that continues a loop before the block belonged to the function, and the bucket loop
of the hash join (`genStaticHashtableIter`) loads the next node there: the IRBuilder takes the alignment
of a load from the module's data layout, which it reaches through the block's function. The block now
joins the function when the body ends (`codegen/CodeGen.cpp`); the loops of `Hashtable.cpp` (`GroupBy`)
took the same path. A join whose build side its selections left empty sized its hash table 2^⌈log2 0⌉ and
threw `std::bad_array_new_length`; the table has at least one bucket now (`StaticHashtable.cpp`).

* A table without an alias is bound by its name (`a.id`), and an unqualified column is ambiguous only if
  a query uses it and it belongs to several tables, as in PostgreSQL. Before, tables without aliases could
  not share a column name, and their names were no bindings.
* Tables that no equality joins with the others are joined as cross products, and conditions over
  several tables apply above the joins. Before, the joins grew from one table along the equalities and
  left the other tables out, which gave wrong results (`FROM a, b` without a condition, one row).
* `NULL` matches no key, not even `NULL`. Joins keep the serial code; aggregates over joins remain
  unsupported (aggregates are queries over the branches of one table).

Checks: `DialectTest.JoinsOfTables` (with the parallel scans of the test and `TARDISDB_VERIFY_IR=1`),
joins of both drivers in `test/test_pgserver.py`.

# Arrays, double precision and expressions

Arrays of PostgreSQL: columns of any element type with `[]` or `ARRAY` (`integer[]`, `numeric(6,2)[]`,
`text ARRAY`, also `bool`, `bigint`, `double precision`, `char(n)`, `varchar(n)`, `date`, `timestamp`),
of any number of dimensions (the declared dimensions are not enforced, as in PostgreSQL), with lower
bounds and `NULL` elements, in `native/sql/Array.hpp`.

* **Storage**: a tuple holds a handle of 16 bytes (address and length, as `text`) of an immutable
  representation in the string pool: number of dimensions, widths, lower bounds, the elements (a string
  element is the end of its characters), the `NULL` bitmap, then the characters. Versions, version
  chains, merge bases, `MERGE` and `DIFF` copy handles. The arrays a query computes go to arenas of the
  query (an `ExecutionResource`, as the plan that generated the code is gone when it runs; the workers of
  a parallel scan take chunks of their own); a store into a table moves them to the string pool (the top
  bit of the length marks them), result tuples copy them.
* **Text**: `'{1,2}'`, `'[0:1]={7,NULL}'`, `'{{"a b",NULL},{c,d}}'` (quotes, backslashes, `NULL` in any
  case) in and out; `ARRAY[...]` of constants in `INSERT`, also nested; `COPY`; pgserver sends the
  array types of PostgreSQL (`int4[]` 1007, `float8[]` 1022, `text[]` 1009, ...) and takes them as
  parameters, also in binary (psycopg 3).
* **Expressions** (new: the select list, `WHERE` and `SET` take expressions; before, columns, aggregates
  of columns and the comparisons above): a parser by precedence (`ExpressionParser`; `OR`, `AND`, `NOT`,
  comparisons with `IS [NOT] NULL`, `[NOT] BETWEEN`, `[NOT] IN`, `op ANY/SOME/ALL (array)`, `||`, `@>`,
  `<@`, `&&`, `+ -`, `* /`, unary minus, subscripts, casts `::type` and `CAST(x AS type)`, function calls,
  `ARRAY[...]`), whose tree (`sqlExpressions::Node`) the `ExpressionAnalyser` types: constants take the
  type of the other side, a number the common type of both (`k + 0.5` of an integer `k` is a numeric),
  and numbers are cast to a common type (integer, bigint, numeric, double; `+ - * /` take no bigint yet).
  In `WHERE`, numbers with a point or an exponent go through the expression rather than the predicates of
  a column, which take a constant in the column's type (`k < 1.5` held for no integer `k = 1`). Computed
  columns of the select list (named as in PostgreSQL, `AS` aliases) are a `Map` (now implemented,
  logical and physical); conditions over one table go to its production, those over several above the
  joins; `SET c = expr` is an update expression in the column's type. Queries over several branches and
  `DIFF` take any condition, but columns and aggregates of columns only.
* **Functions and operators**: subscripts `a[i][j]` (`NULL` out of range), slices `a[l:u]`, `a[:u]`,
  `a[l:]`; `array_append`, `array_prepend`, `array_cat`, `||` (array or element on either side),
  `array_length`, `array_lower`, `array_upper`, `array_ndims`, `array_dims`, `cardinality`,
  `array_position(s)`, `array_remove`, `array_replace`, `array_fill`, `array_to_string`,
  `string_to_array`, `trim_array`; `@>`, `<@`, `&&`, `= ANY`, `<> ALL` (three-valued); `=`, `<>`, `<`, ...
  of arrays as PostgreSQL's btree order (elements, then the number of elements, then the shape; `NULL`
  elements equal each other). Each is one physical `ArrayFunction`, a call of the runtime
  (`tardisArrayCall`) with the values, their `NULL` indicators and types; its errors end the statement.
  `unnest` and other set-returning functions are missing.
* **Double precision** (`DoubleID`; `double precision`, `float`, `float8`, `real`, `float4`; before,
  `numeric(18, 6)`): arithmetic (IEEE, division by zero as for integers), casts with the other numbers
  (to integers half to even, to numerics half away from zero, as PostgreSQL), the order of PostgreSQL
  (`NaN` equals `NaN` and is greater than all), the text of PostgreSQL (shortest exact digits, `1e+20`,
  `NaN`, `Infinity`), pgserver's `float8` (701). Aggregates: `min` and `max` in the slots of integers
  (order-preserving keys), `sum` and `avg` in doubles per branch: the difference arrays of the shared
  scan would cancel them (1e20 + 1 - 1e20 - 1 is -1), so such queries take the per-branch strategy.
* Fixed on the way: the parser kept the current token by reference while later tokens could move it
  (`MERGE ... ON CONFLICT` read freed memory once the tokens grew); text constants emitted their
  instructions where the expression was translated, which could be another function than the one using
  it (a parallel scan read garbage); casts from numerics to integers divided by the scale of the target
  (1) instead of the numeric's; casts of arithmetic took the nullability of the other operand; the
  booleans of `t`, `yes`, `on`, `1` and their negations are accepted, as in PostgreSQL.
* Not changed: `COPY` does not quote fields (also text with `;`).

Checks: `test/array_test.cpp` (11 tests: literals and their text, malformed arrays, subscripts and
slices, the functions and operators, expressions, updates with branches, `MERGE` and `DIFF`, `ALTER
TABLE` and `COPY`, computed arrays in parallel scans, double precision with both strategies, arrays
and doubles in joins and as join keys), with
`TARDISDB_VERIFY_IR=1`; `test/test_pgserver.py` (lists of both drivers, arrays and floats in results).

# Common table expressions (`WITH [RECURSIVE]`), set operations and `VALUES`

`WITH [RECURSIVE] name [ ( columns ) ] AS ( query ) [, ...] query`, where a query is `SELECT`s combined
by `UNION`, `INTERSECT` and `EXCEPT`, each `[ALL | DISTINCT]` (`INTERSECT` binds closer, `UNION` and
`EXCEPT` combine from left to right, parentheses group), `VALUES ( ... ) [, ...]` (a `SELECT` per row,
with the columns `column1`, ..., combined by `UNION ALL`) or a `SELECT` without `FROM` (one row).
`QueryCompiler::execute` recognises these statements (`with::parseSetStatement`, which splits the text at
the parentheses and the words `WITH`, `AS`, `UNION`, `INTERSECT` and `EXCEPT` outside strings and
comments, into a tree of `SetQuery`) and runs each `SELECT` as a statement of its own
(`runQuery`), under the latches of the whole statement: the catalog shared, the tables of all its
queries, and their branches (all branches if a query reads `VERSION ALL`, `DESCENDANTS` or `LEAVES`).

* **Temporary tables** (`TemporaryTables`, `foundations/Database.hpp`): tables outside the catalogue that
  only the statements of the current thread see, under names that shadow the tables of the catalogue
  (`Database::getTable` looks there first), read and written in master; their statement holds the
  catalog latch, so no branch is created while they exist. A common table expression is computed into
  one before the queries that follow read it; the row of a `SELECT` without `FROM` is one too
  (`sqlExpressions::oneRowTable`, a name no statement can spell). A query reads a common table
  expression in master whatever the branch of the session or of a transaction (`confine` leaves it
  alone); `VERSION` on it is an error.
* **Recursion**: with `RECURSIVE`, an expression that reads itself has the form of PostgreSQL,
  `non-recursive query UNION [ALL] recursive SELECT`; the recursive `SELECT` reads the working table, the
  rows the previous step added (at first those of the non-recursive query, which may be any query of
  set operations), until a step adds none. Each step compiles
  its queries again (about 2 ms per step for a query of one table) and frees the working table before.
  A recursive query reads its expression once (`recursive reference ... must not appear more than
  once`).
* **Columns**: the names of the first `SELECT` (or of the column list), in the common type of the
  non-recursive `SELECT`s, as PostgreSQL resolves set operations: integer < bigint < numeric (of the greater
  scale, 18 digits) < double precision, strings as `text`; other types must be equal. The recursive
  queries must give these types. All columns may be `NULL`. Rows are copied into the temporary tables
  through the text of their values (`castColumnValue`), so that they own their strings.
* **Set operations** on the rows of their two queries, compared by the values in the column's type (`1`
  and `1.0` of a numeric column are one row, two `NULL`s are equal): without `ALL` each row once, with
  `ALL` as multisets, `UNION ALL` adding the counts of a row, `INTERSECT ALL` taking the lesser and
  `EXCEPT ALL` subtracting the right one (`with::combineRows`). A query of set operations collects the rows
  of each `SELECT` before it combines them. In a recursive expression, `UNION` without `ALL` also drops
  the rows of earlier steps, so that the recursion ends on cycles of a graph.
* `SELECT` without `FROM` works everywhere (`SELECT 1;`); in the `sql` shell, such statements and those
  with `WITH`, `UNION` or `VALUES` run as statements of a session in master.
* Not supported: `ORDER BY` and `LIMIT` (in no query of TardisDB), subqueries in
  `FROM`, data-modifying statements in `WITH`, mutual recursion (an expression reads only those before
  it, and itself); `+ - * /` of bigint and `||` of text, which recursive queries often use, are not in
  the expressions of TardisDB yet.

Checks: `test/cte_test.cpp` (9 tests: `SELECT` without `FROM`, counting, hierarchies over joins, graphs
with cycles, several expressions that read each other or hide a table, `NULL`s, arrays and doubles in
the rows, `UNION` and `VALUES` with the types of their columns, `INTERSECT` and `EXCEPT` with and without
`ALL` and their precedence, branches and a transaction, the errors of PostgreSQL), every query also with
parallel scans and with `TARDISDB_VERIFY_IR=1`; recursion and set operations of both drivers in
`test/test_pgserver.py`.

# Linear algebra on arrays (without BLAS)

Operations on arrays of numbers (`integer`, `bigint`, `numeric`, `double precision` elements),
computed in double precision; the results are `double precision[]` (or `double precision`).

* **Operators**: `+ - * / ^` element by element, of two arrays of one shape or of an array and a number on
  either side (`1 - a`, `0.1 * g`, `a ^ 2`); unary minus; `**` the matrix product, in general the sums over
  the last dimension of the first and the first dimension of the second (the tensor product of
  `tensor.hpp`: a matrix and a vector, two vectors give their dot product as an array of one element);
  prefix `@` the absolute values. `^` binds closer than `* /` (unary minus closer than `^`), and `**` and
  `@`, like the other operators of PostgreSQL that are not arithmetic, less closely than `+ -`: write
  `(x ** w) - y`. `^`, `@` and the functions below also take numbers (`2 ^ 10`, `sig(0)`).
* **Functions**: `transpose(a)` swaps the first two dimensions (of any element type; a vector becomes a
  column), `kroneckerproduct(a, b)` of two matrices, `sig`, `relu`, `relup` (its derivative), `tanh`,
  `abs` of each element, `softmax(a)` over all elements, `softmaxcce(a, labels)` the categorical
  cross-entropy of `softmax(a)` and the labels (a number), `softmaxccep(a, labels)` its gradient
  `softmax(a) - labels`, `highestposition(a)` the offset (from 0) of the first greatest
  element.
* **NULL**: a NULL element gives NULL elements (of `**`, the sums over its row of the first or its
  column of the second); `softmax`, `softmaxcce(p)` and `highestposition` are NULL if an element is.
  Shapes that do not fit, division by zero and arrays that are not of numbers are errors.
* **Without BLAS**: the products run in blocks (64 x 256 x 512) that stay in the caches, whose innermost loop over a row the
  compiler vectorises (`multiply` in `native/sql/Array.cpp`), about 3 GFLOP/s in one thread (a product of
  two 1000 x 1000 matrices in 0.6 s); TardisDB links no BLAS. The functions run in the generated code
  through `tardisArrayCall` (`Function::Elementwise`, `MatrixProduct`, `Kronecker`, `Transpose`, `Map`,
  `Softmax`, `SoftmaxCce`, `SoftmaxCceP`, `HighestPosition`), with their results in the query's arena.
* `softmax` is exp(x) / sum exp(x) and `softmaxcce` the loss; element-wise operations require one shape, and the products, the Kronecker product and the functions handle NULL elements. The aggregates of arrays (`avg(transpose(x) ** d)` of the scripts in `~/nn2sql`) are
  not here: aggregates of TardisDB take columns.

Checks: `test/linear_algebra_test.cpp` (6 tests: the element-wise operators and their precedence, the
products with NULLs and errors, `transpose` and the functions, softmax and positions, a product larger
than the blocks against a plain loop, gradient descent of least squares in a recursive common table
expression), every query also with parallel scans and with `TARDISDB_VERIFY_IR=1`; products of both
drivers in `test/test_pgserver.py`.

# Dolt through their servers (`compare/`, `results/paper/compare_*.csv`)

`compare/run.sh <dir>` builds pgserver with LLVM 7 in the container and runs it there (host network,
cores 0-17, 18 threads per query), starts `dolt sql-server` (Dolt 2.7.2, `DOLT`, data on the SSD, ext4,
cores 0-17) and drives both from cores 18-35 with `compare/compare.py` (psycopg2, pymysql): the same
statements except for the branch syntax (`CALL DOLT_BRANCH`, `DOLT_CHECKOUT`, `DOLT_MERGE`,
`DOLT_BRANCH('-D')`, `AS OF` reads, `UNION ALL` for several branches). Dolt forks from commits, so it
commits every write (`commit_ms`). Data `t(a, b, g)`, 1M tuples, `g` random groups (100 in chain and
fan-out, so an update writes 1 %; 1000 in the loops, as in the traces). Every sum is checked against a
model on both systems. 44 minutes in all.

* Chain: Dolt sums the leaf in 170 to 200 ms at any depth up to 128, TardisDB in 3.9 to 17 ms (growing
  with the chains). An update of 10,000 tuples takes Dolt 0.55 to 0.6 s, TardisDB 23 to 39 ms; a fork
  about 6 ms against 12 to 17 ms (TardisDB copies the bitmap of every relation).
* Fan-out over 256 branches: one query takes Dolt 107 s (`UNION ALL` of `AS OF`, about as long as one
  query per branch), TardisDB 56 ms (branch-shared scan; one query per branch: 8.2 s).
* Loops (total): mcts 44.7 against 444 s, beam 14.8 against 213 s (with the column: `ALTER TABLE`
  7.6 ms against 3.1 s), bisect 22.0 against 115 s. Updates
  dominate Dolt (each `UPDATE ... WHERE g = k` scans the table, as in TardisDB, which has no index on g
  either); TardisDB's largest cost in bisect is dropping the snapshots, which costs Dolt almost nothing.
* Not compared: storage (Dolt on disk, TardisDB in memory) and durability (TardisDB logs nothing).

# Merge policies (`MERGE BRANCH s INTO t ON CONFLICT KEEP TARGET | KEEP SOURCE`)

Without a policy, a merge with a conflict changes nothing (as before). With one, every conflict takes
the target's or the source's side and the other changes are merged: an attribute changed on both
sides gets the target's or the source's value; a tuple deleted in the target and changed in the source
stays deleted or comes back with the source's version; a tuple deleted in the source and changed in the
target keeps the target's version or is deleted. The version of the source is the base of the next
merge, so a resolved conflict does not come back. The result counts the resolved conflicts
(`MergeResult::resolved`); pgserver sends the changed tuples in the tag (`MERGE n`) and the conflicts
as a notice.

The base of a tuple without a recorded merge base was the newest version both branches see. That is
wrong if one of them did not contain the tuple when they split: a version from before a delete is
still in the chain and still seen. The policies made this reachable (a tuple deleted in a branch comes
back by a merge, and a later fork shares the deleted state), and it also hit merges without a policy.
Now a version is the base only if both sides contained the tuple at the split: a side that lacks the
tuple and has not written it since did not contain it then, and the common path must not have deleted
the tuple since the version. For the latter, every identifier keeps the tuples whose last write under it
was a delete (`Table::_deleted`, freed when its branch is dropped; the identifiers from the version's
down to the one at which the two sides split wrote later). Without a base, a tuple the target
has alone stays, and one the source has alone is inserted.

`Table::_deleted` grew when the first delete under an identifier arrived (`record_write`), while
writers of other branches, which hold the table shared, inserted into and erased from the sets of their
identifiers: the vector moved the sets under them, a use after free that crashed
`ConcurrencyTest.BranchesInParallel` about once in a few hundred runs (ThreadSanitizer showed it). Like
`Table::_writes`, the sets of all identifiers now exist before any write (`Database::newIdent` and new
tables size both), and `record_write` no longer grows either (`ConcurrencyTest.WriteListsExistBeforeWrites`).

Checks: `MergePoliciesResolveConflicts`, `MergePoliciesResolveDeleteChangeConflicts`, and the randomised
comparison with the model, whose merges now take a random policy (one third each); it found both
flaws of the base (seeds 1 and 4) and passes with 60 seeds.

# The lineage map compiled into the scans (`COMPILED_LINEAGE`)

Question: is the speedup of the labels in the chain figure the labels, or only that their test is compiled into
the scan while the lineage map is a call into C++ with a hash lookup per version? `COMPILED_LINEAGE`
(with `INTERVAL_VISIBILITY=OFF`) keeps TardisDB's rule, but as a dense array per reader over all
branches: a version written by b at creation_ts is visible iff creation_ts < limits[b] (the maximum
for the reader, the id of the child on the path for an ancestor, 0 otherwise; `Database::lineageLimits`).
Tables keep `_head_stamp` (branch and time of the in-place version if it heads the chain, 8 bytes per
tuple) beside `_head_ident`, so the scan has the same in-place fast path as with labels; the chain walk
uses the limits too. In the branch-shared scan, "read by all" compares with the least limit of the
readers, and the chain walk tests every waiting reader per version, since without labels the readers of
a version need not be adjacent. Checks: tests, verify with and without `--crossbranch`.

Results (results/clineage, run.sh "depth crossbranch extra", 18 threads, 1 M tuples). The chain and fan-out figures
of the paper add the compiled lineage from this run (mt18_clineage_*.csv, mt18_cb_clineage_*.csv, copied into
results/paper); their other curves stay from the paper run, with which this run agrees within noise
(labels, k = 256: 54.5 against 54.8 ms; depth 256 with updates: 9.8 against 9.6 ms).
* Chain: the compiled lineage scans as fast as the labels, at every depth, with and without
  updates (no updates: 0.95 to 1.15 ms against 1.0 to 1.3 ms; the lineage map 4.3 to 4.7 ms). The gap
  without updates is the call and the hash lookup, not the labels.
* Fan-out, k scans in one query or per branch: the same for both.
* Branch-shared scan: close up to k = 64 (1 % updates: 26 against 21 ms), then the k tests per version
  grow: k = 256 88 against 55 ms, k = 1024 1507 against 204 ms (0.1 %: 198 against 36 ms). The labels
  turn the readers of a version into one range (two binary searches); the lineage needs one test per
  reader. The order of the readers is arbitrary here; a post-order numbering of the branch tree per
  query would make the readers of a version adjacent again, which is what the labels maintain.

# Lazy membership in the paper setup (`run.sh "loops fork"`, results/paper)

The loops were re-measured with a fourth build, interval labels with lazy membership
(`/b4`, `-DLAZY_MEMBERSHIP=ON`); the traces are identical to the earlier run. Medians of 3 runs,
18 threads, 1 M tuples (s; membership in MiB):

| | lineage | labels | labels, lazy |
|---|---|---|---|
| mcts: total / fork / drop, merge | 46.5 / 6.4 / 5.2 | 43.0 / 6.1 / 5.1 | 29.8 / 0.018 / 0.43 |
| beam: total / fork / drop, merge | 19.2 / 3.7 / 4.1 | 14.0 / 3.7 / 4.3 | 6.0 / 0.008 / 0.41 |
| bisect: total / fork / drop, merge | 20.6 / 2.7 / 15.7 | 20.8 / 2.1 / 16.6 | 13.8 / 0.006 / 11.6 |
| membership after the loop | 61 | 61 | 1.0 |

With bitmaps, a fork copies the bitmap column of its parent and a drop clears the column; lazy
membership does neither (a byte per tuple, entries only for deletes). Fork alone (`fork_eager.csv`,
`fork_lazy.csv`, median of 7): 1 M tuples 11.1 ms against 0.40 µs, 10 M tuples 71 ms against 0.33 µs.
bisect still spends most of its time dropping: the collection examines every tuple main changed
after the fork of a snapshot. The labels gain little over the lineage in mcts (this run 46.5 against
43.0 s, earlier runs 43.8 / 44.1 and 48.7 / 44.7 s).

# Garbage collection in the paper setup (`run.sh gc`, results/paper/gc_*.csv)

`--mode=cycles` reports `versions_mib` now: the memory of the versions in chains as allocated
(`live_version_bytes()`, `malloc_usable_size`), beside RSS; 40 bytes per version of `t(a, b)` (32-byte
header). 1 M tuples, 200 cycles (fork from main, 1 % updates in the branch and in main, scan, drop),
18 threads; after 200 cycles:

| | no collection | collection | collection, long-lived branch |
|---|---|---|---|
| versions / their memory | 2 000 000 / 76 MiB | 0 / 0 | 864 614 / 33 MiB |
| RSS, lazy / eager | 303 / 330 MiB | 182 / 224 MiB | 247 / 275 MiB |
| membership, lazy / eager | 1 / 30.5 MiB | 1 / 30.5 MiB | 1 / 30.5 MiB |
| drop, lazy / eager | 12 / 23 ms | 10 / 23 ms | 16 / 28 ms |

Eager membership never reuses the bitmap columns of dropped branches (30.5 MiB after 200 cycles).
Agent loops with lazy membership, with / without collection (`gc_loops_lazy_gc*.csv`; `--gc=false` also
turns off overwriting versions of the current identifier): versions left 0 / 5 000 (mcts), 0 / 49 000
(beam), 0 / 256 000 (bisect, 13.8 MiB); drop and merge 0.44 / 0.26 s, 0.42 / 0.37 s, 13.0 / 3.4 s. The
dropped branches' own versions are freed in both cases; collection frees the versions of main that only
dropped branches read, which in bisect costs most of the drop time. The garbage collection figure of the paper plots the lazy build.

# The paper's results with LLVM 18 (`results/paper/`)

`results/paper/` was replaced by one run of `NATIVE=1 run.sh <dir> "paper scaling extra check plot"`
from a clean checkout (68 minutes, check passed in all four builds) and one of `NATIVE=1 compare/run.sh`
(43 minutes), GCC 13.3, LLVM 18.1.3. The same run in the container (LLVM 7) agrees within run-to-run
variation. Against the earlier LLVM 7 files: labels against the compiled lineage in the branch-shared
scan over 1024 branches 200.6 against 1523.9 ms (7.6x; before 7.3x), the leaf at depth 256 with a version
map 3.55 against 9.59 ms (before 3.51 against 9.64 ms); loops within 0 to 16 % (lineage mcts 50.4 against
46.5 s); Dolt within noise.
