# TardisDB in BranchBench

`tardisdb/` is a system package for the BranchBench benchmark engine
([github.com/ElaineAng/db-fork](https://github.com/ElaineAng/db-fork)): an adapter, a connector and
lifecycle scripts, as its `CONTRIBUTING.md` describes. It drives TardisDB through `pgserver`, the
PostgreSQL-compatible server of this repository, with psycopg2.

Status: the package has not been run inside the engine yet (its environment is not installed here).
What it relies on was checked against pgserver directly: the CH-benCHmark schema and seed of the engine
load (12 tables, the seed in 1.7 s), the statements of the microbenchmark run (point and range reads on
the composite key `(c_w_id, c_d_id, c_id) >= (...)`, inserts with all columns, updates, deletes,
`SELECT MAX(...)`), and psycopg2 and psycopg 3 connect.

## Installing

```
# TardisDB (branch branch-native)
cmake -DCMAKE_BUILD_TYPE=Release -DINTERVAL_VISIBILITY=ON -DTARDISDB_LLVM_VERSION=18 <TardisDB> && make pgserver
# the engine, with the package linked in
git clone https://github.com/ElaineAng/db-fork && cd db-fork
ln -s <TardisDB>/benchmark/branchbench/tardisdb dblib/systems/tardisdb
uv sync --frozen
export TARDISDB_PGSERVER=<build>/pgserver DBFORK_STATE_DIR=$PWD/.state
dblib/systems/tardisdb/start && dblib/systems/tardisdb/check
uv run --frozen python -m microbench.runner2 --config <task config with backend tardisdb> --output-dir run_stats/tardisdb
```

Environment: `TARDISDB_PGSERVER` (the binary), `TARDISDB_HOST` (127.0.0.1), `TARDISDB_PORT` (55433),
`TARDISDB_USER`, `TARDISDB_THREADS` (threads per query; all cores).

## What maps to what

| Engine | TardisDB |
|---|---|
| `branch(name, from_ref)` | `CREATE BRANCH name FROM parent`: a metadata record; only from a branch head |
| connect to a ref | `SET branch = name`; statements without `VERSION` run in the session's branch |
| `merge(into, source, on_conflict)` | `MERGE BRANCH source INTO into ON CONFLICT KEEP TARGET` ("ours") or `KEEP SOURCE` ("theirs"), three-way per attribute; a resolver callable is unsupported |
| `diff(a, b)` | `SELECT * FROM DIFF(t VERSION a, t VERSION b)` per table, counted by primary key |
| `delete(ref)` | `DROP BRANCH name`; a branch with descendants cannot be dropped |
| multi-ref `exec` | `db.table(ref, t)` is `t VERSION ref` |
| `commit`, `log`, `rebase`, `revert`, `reset` | unsupported: TardisDB has branches, but no commits |
| catalog | `SHOW TABLES`, `SHOW COLUMNS FROM t` (TardisDB has no `information_schema`) |
| storage | resident memory of pgserver (`STORAGE_SCOPE = "server"`) |

The macrobenchmark scenarios S1 to S6 commit, so they end as unsupported at their first commit. The
connector skips the statements of a dump that TardisDB does not have and that hold no data (foreign
keys, `CREATE INDEX` of PostgreSQL, `DROP TABLE`, `SET`) and says how many it skipped.

## SQL of TardisDB (as of this branch)

Supported: `CREATE TABLE` with the type names of PostgreSQL (`smallint`, `int`, `bigint`, `decimal`,
`numeric`, `char`, `varchar`, `text`, `boolean`, `date`, `timestamp`; `float`, `real` and `double` are
`numeric(18, 6)`) and `PRIMARY KEY` (recorded, not enforced); multi-row `INSERT`; `WHERE` with
conjunctions of `=`, `<>`, `<`, `<=`, `>`, `>=`, `BETWEEN`, `IN` and row values; joins on `=`;
`count`, `sum`, `min`, `max`, `avg`, `GROUP BY branch`; the branch statements.

Not supported: `NULL` values (columns declared nullable take none: versions hold no null indicators),
`OR`, `ORDER BY`, `LIMIT`, expressions in the select list, `DROP TABLE`, foreign keys, transactions
over several statements (statements autocommit; `ROLLBACK` after a write is an error).
