# TardisDB with branch-native MVCC

This branch (`branch-native`) makes branches native to TardisDB's multi-version concurrency control:
interval labels decide visibility, one scan answers queries over many branches, and branches merge,
change their schema and are garbage-collected when dropped. `pgserver` serves PostgreSQL clients.
It contains the code and the evaluation of the paper *No Time to Branch: Multi-Version Concurrency
Control for Agentic Database Workloads* (Maximilian E. Schüle, ICDE 2027, DEFT track).

## Building with LLVM 18

Tested on Ubuntu 24.04 with GCC 13.3, LLVM 18 and Boost 1.83:

```
sudo apt install cmake g++ llvm-18-dev libboost-all-dev libreadline-dev zlib1g-dev libtinfo-dev
git clone https://github.com/tum-db/TardisDB.git && cd TardisDB && git checkout branch-native
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DINTERVAL_VISIBILITY=ON -DTARDISDB_LLVM_VERSION=18 ..
make -j branchBench tester sql pgserver
```

| Target | |
|---|---|
| `sql` | interactive shell over an empty in-memory database (see [SQL shell](#sql-shell)) |
| `pgserver` | server for PostgreSQL clients: `pgserver -p 55433`, then `psql -h 127.0.0.1 -p 55433` |
| `branchBench` | the benchmark driver of the paper (`--mode=depth`, `crossbranch`, `agentic`, `fork`, `cycles`, `verify`, ...) |
| `tester` | the test suite; it passes except for `InteropTest.*`, which crashes on the original TardisDB as well |

The paper compares four builds, selected by CMake options:

| Build | CMake options | Variant |
|---|---|---|
| lineage | `-DINTERVAL_VISIBILITY=OFF` | TardisDB's lineage map, a call into C++ per version |
| interval | `-DINTERVAL_VISIBILITY=ON` | interval labels, with a membership bitmap per branch |
| clineage | `-DINTERVAL_VISIBILITY=OFF -DCOMPILED_LINEAGE=ON` | the lineage compiled into the scans |
| lazy | `-DINTERVAL_VISIBILITY=ON -DLAZY_MEMBERSHIP=ON` | interval labels with lazy membership |

A first measurement, the scan of the leaf of a branch chain over its depth:

```
./branchBench --mode=depth --n=1000000 --frac=0.01 --runs=5 --threads=18 --depths=0,1,2,4,8,16,32,64,128,256
```

## Reproducing the paper's evaluation

`benchmark/branch-native/run.sh` builds the four variants, runs the benchmarks and plots every
evaluation figure except the comparison with Dolt into `figures.pdf`. It runs natively with LLVM 18,
the environment of the paper's numbers, or in a Docker container with LLVM 7. Both give the same
results within run-to-run variation (`benchmark/branch-native/results/llvm/`).

```
NATIVE=1 benchmark/branch-native/run.sh ~/tardisdb-results     # natively, LLVM 18
benchmark/branch-native/run.sh ~/tardisdb-results              # in the container, LLVM 7
```

The output directory gets the builds (`build/`), the CSVs and `figures.pdf`. Compare `figures.pdf`
with the paper, or the CSVs with ours in `benchmark/branch-native/results/paper/` (same file names and
columns). Either way takes about 78 minutes on the machine of the paper
([below](#machine-of-the-paper)), builds included, and 90 minutes with the phases `extra` and `check`.

Requirements:

* Linux on x86-64, a git checkout (the scripts locate the source with `git rev-parse`), Python 3
  (standard library only), and `pdflatex` with `pgfplots` for the figures (on Debian and Ubuntu:
  `texlive-latex-extra`, `texlive-pictures`, `texlive-fonts-recommended`).
* Natively: the packages of [Building with LLVM 18](#building-with-llvm-18); `LLVM_VERSION` selects
  another LLVM. In the container: Docker usable without `sudo`; `Dockerfile` installs Ubuntu 20.04,
  GCC 9.4, LLVM 7 and Boost 1.71, mounts the source read-only and writes only to the output directory.
* 18 cores and 36 hardware threads for the paper setup, 8 GiB of memory, and 350 MB of disk for the
  builds. On a smaller machine, see [Fewer cores](#fewer-cores): the numbers then differ, but the shape
  of the figures should not.

### Phases

`run.sh <output directory> [phases]` runs the phases of `bench.sh`, pinned to the cores below, and
prints the time of each; the times below were the same natively and in the container. The default is
`paper scaling plot`; phases can be combined in any order, e.g. `run.sh ~/r "depth plot"`, and `plot`
alone skips the builds.

| Phase | Measures | Builds | Cores | Time |
|---|---|---|---|---|
| `paper` | `depth crossbranch loops fork gc` | | 0-17 | 25 min |
| `depth` | scan of the leaf of a branch chain over its depth, with 0 % and 1 % updates per level | lineage, clineage, interval (also with a version map) | 0-17 | 3 min |
| `crossbranch` | sum per branch over k branches forked from `main`: k queries, k scans in one query, branch-shared scan | interval, clineage | 0-17 | 10 min |
| `loops` | the agent loops mcts, beam and bisect in SQL, per phase, `LOOP_RUNS` (3) times; writes the traces | lineage, interval, lazy | 0-17 | 12 min |
| `fork` | latency of a fork over the table size, with bitmaps and with lazy membership | interval, lazy | 0-17 | <1 min |
| `gc` | versions after every snapshot and drop of bisect and the time of each, and the agent loops at their end, with and without collection | lazy | 0-17 | 2 min |
| `scaling` | the scans of `depth` (all variants) and `crossbranch` (interval, clineage) with 1, 2, 4, 8, 16, 18, 32 and 36 threads | lineage, clineage, interval (also with a version map) | 0-35 | 48 min |
| `plot` | `summarize.py` (medians, speedups, the excerpt of the listing), then `plots/plot.sh` | | | <1 min |
| `extra` | measured, not plotted: min/max, the lineage map across branches, fork without labels, `ALTER TABLE` | lineage, interval | 0-17 | 5 min |
| `check` | `tester` and `branchBench --mode=verify` against a reference model (serial, 8 and 36 threads, branch-local columns); output in `check.txt` | all | 0-35 | 9 min |

The four builds take 3 minutes. Single queries are medians of 5 runs after a warm-up, forks of 7, the
loops of `LOOP_RUNS` runs.

### Which file gives which figure

Figure numbers are those of the submitted paper. The paper inputs the figure sources of
`benchmark/branch-native/plots/` unchanged (as `figures/eval-*.tex`, with `\datadir` pointing to its
copy of the results).

| Paper | Figure source | Phase | Files | Columns |
|---|---|---|---|---|
| Figs. 6 and 9: branch trees of the benchmarks | `lineage.tex` (drawn) | | | |
| Fig. 7a: scan of the leaf of a branch chain over its depth (the paper shows 1 % updates) | `depth.tex` | `depth` | `mt18_{lineage,clineage,interval}_frac{0,0.01}.csv`; with a version map `mt18_interval_vmap_frac{0,0.01}.csv` | `depth`, `scan_leaf_ms`, `scan_main_ms` |
| Fig. 8a: sum per branch over k branches (the paper shows 1 % updates) | `crossbranch.tex` | `crossbranch` | `mt18_cb_{interval,clineage}_frac{0.01,0.001}.csv` | `k`, `separate_ms` (k queries), `perbranch_ms` (k scans in one query), `shared_ms` (branch-shared scan) |
| Figs. 7b and 8b: speedup over threads (the paper splits the curves by benchmark) | `scaling.tex` | `scaling` | `scaling_{cb,depth}_t<threads>.csv` and `scaling_{depth_lineage,depth_clineage,depth_vmap,cb_clineage}_t<threads>.csv`, collected in `scaling.csv` | `threads`, `*_speedup` |
| Listing 2: excerpt of `beam` | | `loops` | `trace_beam.sql`, cut into `trace_beam_excerpt.sql` | |
| Fig. 10: agent loops per phase | `loops.tex` | `loops` | `loops_{lineage,interval,lazy}_r{1,2,3}.csv`, medians in `loops.csv` | `fork_s`, `mutate_s` (update), `evaluate_s`, `resolve_s` (drop, merge), `compile_s`, `membership_mib` |
| Fig. 11: garbage collection in bisect | `gc.tex` | `gc` | `gc_bisect_lazy_gc{true,false}.csv` | `step` (256 snapshots, then 256 drops), `versions_mib`, `ms` (time of the step) |
| Fig. 12: TardisDB and Dolt through their servers | `compare.tex` | `compare/run.sh` | `compare_{chain,fanout}_{tardisdb,dolt}.csv`, `compare_loops_{mcts,beam,bisect}_{tardisdb,dolt}.csv`, collected in `compare_loops.csv` | `leaf_sum_ms`; `one_query_ms`, `per_branch_ms`; phases in s |
| Text of V-B and V-C | | `fork`, `gc` | `fork_{eager,lazy}.csv`, `gc_bisect_lazy_gc{true,false}.csv` | |
| not in the paper | | `extra`, `gc` | `mt18_cb_interval_minmax_frac0.01.csv`, `mt18_cb_lineage_frac0.01.csv`, `fork.csv`, `schema.csv`, `gc_loops_lazy_gc{true,false}.csv` | |

To rebuild the paper's figures from our results without running anything:

```
cp -r benchmark/branch-native/results/paper ~/paper-results
benchmark/branch-native/plots/plot.sh ~/paper-results        # ~/paper-results/figures.pdf
```

`results/paper/` holds one run of `NATIVE=1 run.sh <dir> "paper scaling extra check plot"` and one of
`NATIVE=1 compare/run.sh <dir>` from a clean checkout (LLVM 18, November 2026); `scaling` was then rerun
with the same builds for 1, 2, 4, 8, 16, 18, 32 and 36 threads, and extended by the other variants of the chain and the fan-out with compiled lineage. `gc_bisect_lazy_gc{true,false}.csv`
were measured afterwards with the same builds; the earlier create-and-drop cycles (`--mode=cycles`) are in `results/gc/`.

### The comparison with Dolt (Fig. 12)

```
NATIVE=1 DOLT=<path to dolt> PYTHON=<python with psycopg2 and pymysql> benchmark/branch-native/compare/run.sh ~/tardisdb-results
benchmark/branch-native/plots/plot.sh ~/tardisdb-results
```

`compare/run.sh` builds `pgserver` natively (or, without `NATIVE=1`, in the container) and runs it on
cores 0-17, starts
`dolt sql-server` on the same cores (Dolt 2.7.2 in the paper, from
[github.com/dolthub/dolt/releases](https://github.com/dolthub/dolt/releases)), and drives both from
cores 18-35 with `compare/compare.py`. The loops replay the traces of the `loops` phase (by default
those in `results/paper/`; a second argument names another directory). It takes 44 minutes; run it
into the same directory as `run.sh` to get all figures in one `figures.pdf`. Environment: `CORES`,
`CLIENT_CORES`, `THREADS`, `N`, `DEPTHS`, `KS`, `RUNS`, `MODES`, `TOPOLOGIES` (see the script).

### Fewer cores

| Variable | Default | Meaning |
|---|---|---|
| `CORES` | `0-17` | cores of all phases but `scaling` and `check` |
| `THREADS` | `18` | threads per query in those phases |
| `ALL_CORES` | `0-35` | cores of `scaling`, `check` and the builds in the container |
| `SCALING_THREADS` | `1 2 4 8 16 18 32 36` | thread counts of `scaling` |
| `LOOP_RUNS` | `3` | runs of each loop |
| `JOBS` | `16` | parallel build jobs |

For example, natively on 8 cores:
`NATIVE=1 CORES=0-7 THREADS=8 ALL_CORES=0-7 SCALING_THREADS="1 2 4 8" JOBS=8 benchmark/branch-native/run.sh ~/r8`.

### Contents of `benchmark/branch-native/`

| Path | Content |
|---|---|
| `run.sh`, `bench.sh` | the paper's measurements: `run.sh` builds and runs the phases of `bench.sh` |
| `summarize.py` | medians of the loop runs, speedups over threads, the excerpt of the listing |
| `plots/` | the figures of the evaluation (pgfplots/TikZ), `figures.tex` and `plot.sh` |
| `compare/` | the comparison with Dolt: `run.sh` and the driver `compare.py` |
| `Dockerfile` | the build and run environment of the paper |
| `results/paper/` | the CSVs and traces of the paper |
| `results/<experiment>/` | further measurements, one directory per experiment (`fork`, `depth`, `agentic`, `crossbranch`, `clineage`, `llvm`, `diffmerge`, `gc`, `concurrent`) |
| `experiments.sh` | reruns those experiments natively with LLVM 18, eager and lazy membership |
| `asis/` | the measurements of unmodified TardisDB (commit `0d805c4`) |
| `visbench.cpp` | standalone comparison of the lineage-map test and interval labels |
| `AGENTIC_BENCHMARK.md` | design of the agentic benchmark (`branchBench --mode=agentic`) |
| `NOTES.md` | what changed in TardisDB for each experiment, how it was checked, what it measured |

### Machine of the paper

Ubuntu 24.04.5 LTS (Linux 7.0), Intel Xeon W-2295 (3.00 GHz, 4.60 GHz turbo, 18 cores, 36 hardware
threads, 24.75 MiB L3), 128 GiB DDR4 ECC at 2933 MT/s, 512 GB NVMe SSD; all data resides in main memory.
The paper's numbers were measured natively with GCC 13.3 and LLVM 18.1.3.

## Branch-aware SQL

The statements run in `sql`, in `pgserver` and in `branchBench`:

```sql
CREATE BRANCH b17 FROM main;
UPDATE stock VERSION b17 SET s_quantity = s_quantity - 5 WHERE s_i_id = 42;
CREATE INDEX ON stock (s_i_id) VERSION b17;
ALTER TABLE customer VERSION b17 ADD COLUMN c_tier INTEGER DEFAULT 0;
SELECT branch, sum(ol_amount) FROM order_line VERSION ALL DESCENDANTS OF main GROUP BY branch;
SELECT * FROM DIFF(customer VERSION b17, customer VERSION main);
MERGE BRANCH b17 INTO main ON CONFLICT KEEP SOURCE;
DROP BRANCH b17 CASCADE;
```

`benchmark/branch-native/NOTES.md` describes their semantics and limits;
`test/branch_statements_test.cpp`, `test/schema_test.cpp` and `test/dialect_test.cpp` test them,
including randomised comparisons with a reference model. Statements may be issued from several threads
at once (one agent per branch); NOTES.md, concurrent clients, describes what excludes what. In
`pgserver`, a transaction block (`BEGIN` ... `COMMIT`) has snapshot isolation in a private branch that
`COMMIT` merges back and `ROLLBACK` drops. Columns declared without `NOT NULL` take `NULL` values in
every branch: `WHERE` has SQL's three-valued logic, aggregates skip `NULL`s, and `MERGE` and `DIFF`
treat two `NULL`s as equal (`test/null_test.cpp`). Columns may be arrays of PostgreSQL (`integer[]`,
`text[]`, ... of any number of dimensions, with their functions and operators) and of type
`double precision`; the select list, `WHERE` and `SET` take expressions (`test/array_test.cpp`).
Queries may start with `WITH [RECURSIVE]` and combine `SELECT`s with `UNION`, `INTERSECT` and
`EXCEPT` (each also with `ALL`); `VALUES` and
`SELECT` without `FROM` are queries too (`test/cte_test.cpp`).
Arrays of numbers have linear algebra (`**` for matrix products, `transpose`,
`kroneckerproduct`, `sig`, `softmax`, ...), computed without BLAS (`test/linear_algebra_test.cpp`);
`sum` and `avg` add arrays element by element, and aggregates take expressions, appear in expressions,
read joins and common table expressions, group by columns, and `HAVING` filters the groups
(`test/aggregate_test.cpp`).

## SQL shell

`sql` starts an empty in-memory database with an interactive prompt; `sql FILE...` executes the files and quits.
Statements end with `;` and may span several lines. The arrow keys scroll through earlier statements,
which are kept in `~/.tardisdb_history`. `\?` lists the backslash commands: `\q` (quit), `\i FILE`
(execute a file), `\d` and `\d TABLE` (tables and their columns), `\b` (branches) and `\timing`.

## TardisDB

The following preprocessor options control the build: `USE_INTERNAL_NULL_INDICATOR`, `USE_LLVM_SELECT`,
`USE_NEW_EXTRACT`, `LOAD_NULLABLE`. `-DTARDISDB_LLVM_VERSION=<major>` selects the LLVM version; without it,
CMake builds against the first installed of LLVM 18 and 7. The REST server (`server`) is built only if the
Pistache HTTP library is found (`PISTACHE_INCLUDE`, `PISTACHE_LIBRARY_PATH`).
The `llvm-prototype` executable expects a directory named `tables` with the database data inside the
build directory. The default SQL schema, `schemas/schema.sql`, is compiled in like any other source
file, so a rebuild reflects any change to it.
