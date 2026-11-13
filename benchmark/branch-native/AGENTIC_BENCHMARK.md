# An agentic branching benchmark: branch-mutate-evaluate loops

Agents use a database as working memory: they fork a branch, mutate it, evaluate it with
queries, and keep, merge or discard it. The benchmark replays these loops with the branch
topologies of the workloads in Table I of the paper and measures what each requirement R1–R6
asks of the system.

## Design principles

* **Loops, not single operations.** A run is a sequence of loop iterations; each iteration
  consists of the phases *fork*, *mutate*, *evaluate* and *resolve* (keep, merge or drop).
  Every phase is timed separately, so a system that is fast to fork but slow to read shows
  where it loses.
* **Topology as the main parameter.** Width, depth, lifetime and fate of branches follow
  from the agent's search strategy, not from a fixed schedule.
* **Deterministic.** A seed fixes the tree, the mutations and the scores, so that systems
  replay exactly the same trace. The scores are drawn from the seed, not from query results,
  so that a faster or slower system does not change the search.
* **Replayable by other systems.** A run can be exported as a trace of SQL statements per
  branch, which adapters replay on Dolt, Neon, Xata, PostgreSQL (savepoints and clones),
  DuckDB `differential`, MatrixOne and Chronos.
* **Checked.** Every branch is compared with a reference model in which a fork copies its
  parent's state (as `branchBench --mode=verify` does for TardisDB). A system that is fast
  but wrong fails the run.
* **Coverage is a result.** Operations a system lacks (merge, drop, branch-local schema,
  cross-branch queries) are reported as unsupported, not silently skipped.

## Data and operations

* Schema and data: TPC-C (mutations) and TPC-H (evaluations), as in BranchBench, at scale
  factors chosen so that the database is 1–100 GB.
* Mutations: TPC-C transactions (NewOrder, Payment, Delivery) or updates of a fraction of
  tuples; schema migrations (`ADD COLUMN` with backfill, `CREATE INDEX`) for R6.
* Evaluations: TPC-H-like aggregates (Q1, Q6) or point and range queries on the branch;
  for R4, the same evaluation over all candidates in one query.

## Topologies

| Topology | Loop | Parameters | Stresses |
|---|---|---|---|
| **beam** (simulation, best-of-n) | fork W candidates from the current best, mutate and evaluate each, keep the best, drop the rest | W, rounds, mutation size | R2, R3, R4 |
| **mcts** (tree search) | select a node by UCB, fork, mutate, evaluate, backpropagate; prune subtrees with low value | width, iterations, prune threshold | R1, R3 |
| **bisect** (failure reproduction) | main advances through H commits; fork a probe from the middle snapshot, run the test, recurse | H, test cost | R1, R2 |
| **migrate** (software engineering) | bushy tree: each branch applies a schema migration and backfill, runs a test suite, the passing branch is merged | fan-out, migration mix | R5, R6 |
| **curate** (data curation) | k agents enrich disjoint or overlapping attributes of the same tuples and merge back | k, attribute overlap | R4, R5 |

## Metrics

* Time per phase (fork, mutate, evaluate, resolve) and per loop; loops per second.
* Evaluation latency over branch depth (R1) and fork latency over database size (R2).
* Memory over branches created vs. branches surviving (R3).
* Cost of comparing k branches relative to k single scans (R4).
* Merge latency over divergence; conflicts reported vs. true conflicts at attribute
  granularity (R5).
* Coverage matrix of supported operations per system.

## Two modes

1. **Scripted** (reproducible): the driver generates the loops from the seed.
2. **Agent-driven** (usability): an LLM agent solves a task suite with the branch operations
   exposed as tools through the Model Context Protocol; we record the operation trace it
   produces and replay it in scripted mode on all systems.

## What is implemented now (`branchBench --mode=agentic` in TardisDB)

`beam`, `mcts` and `bisect` run as loops of SQL statements only, with all four phases:

* Data: `t(a, b, g)` of `--n` tuples; `g` divides them at random into groups of `--mutations` tuples.
* Fork: `CREATE BRANCH x FROM p`. Mutate: `UPDATE t VERSION x SET b = v WHERE g = k` (one group).
* Schema: `beam` first forks a workspace from `main` that adds a column (`ALTER TABLE t VERSION w ADD
  COLUMN score INTEGER DEFAULT 0`); its candidates write `b` and `score`, and the final merge takes the
  column over into `main`.
* Evaluate: `SELECT sum(b) FROM t VERSION x`; `beam` evaluates the candidates of a round in one query,
  `SELECT branch, sum(b) FROM t VERSION (x1, ..., xW) GROUP BY branch`.
* Resolve: `beam` drops the candidates it does not keep, `bisect` drops each probe after its test and
  the snapshots at the end; `mcts` and `beam` merge their result into `main` (`MERGE BRANCH x INTO main`)
  and then drop their tree (`DROP BRANCH ... CASCADE`).
* Scores come from the seed, not from the query results, so that every build issues the same
  statements. Every sum (and `main` after the merge) is checked against a model in which a branch is
  the value of `b` per group; a wrong result aborts the run.
* `--trace=<file>` writes the statements; the paper's listing is an excerpt of the `beam` trace
  (`summarize.py` cuts it).
* Each phase is timed without the compilation of its statements, which is reported as `compile_ms`.

Not yet: `migrate` and `curate`, TPC-C/TPC-H data, and adapters for other systems.

For R4, `branchBench --mode=crossbranch` measures the comparison step of a beam round in isolation:
k candidates forked from `main` mutate a fraction of the tuples, and one aggregate per candidate is
computed by k queries, by one query with k scans and by one query with the branch-shared scan
(`SELECT branch, sum(b) FROM t VERSION ALL DESCENDANTS OF master GROUP BY branch`).
