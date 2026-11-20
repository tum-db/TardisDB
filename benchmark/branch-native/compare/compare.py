"""TardisDB and Dolt through their SQL servers, with the same driver: TardisDB's pgserver (PostgreSQL
protocol, psycopg2) and dolt sql-server (MySQL protocol, pymysql).

Data: t(a, b, g) with a unique key a, b = 0 and g dividing the tuples at random into groups; an update
writes b of one group. Every sum is checked against a model in which a branch is the value of b per
group (unset groups are 0).

  chain   fork a branch from the previous one D times; after each fork, the new branch and main update
          one group each (1 % with 100 groups); at the depths of --depths, sum b on the leaf and on main
  fanout  fork k branches from main, each updates one group; at the counts of --ks, sum b per branch with
          one query (TardisDB: VERSION ALL DESCENDANTS OF main; Dolt: UNION ALL of AS OF reads) and
          with one query per branch
  loops   replay a trace of branchBench --mode=agentic (its SQL, including the column beam adds), per phase

Dolt forks from commits: every write is committed (CALL DOLT_COMMIT) before the next statement, so
that later forks see it; the commits are timed as a phase of their own.

Usage: compare.py --system tardisdb|dolt --mode chain|fanout|loops [options] > result.csv
"""

import argparse
import random
import re
import statistics
import sys
import time


class Model:
    """b per group for every branch"""

    def __init__(self, groups: int, group_size: int):
        self.groups, self.group_size = groups, group_size
        self.branches = {"main": {}}
        self.scores = {}  # score per group, of the branches with the column

    def fork(self, name: str, parent: str):
        self.branches[name] = dict(self.branches[parent])
        if parent in self.scores:
            self.scores[name] = dict(self.scores[parent])

    def add_column(self, branch: str):
        self.scores[branch] = {}

    def update(self, branch: str, group: int, value: int, score=None):
        self.branches[branch][group] = value
        if score is not None:
            self.scores[branch][group] = score

    def merge(self, source: str, target: str):
        self.branches[target] = dict(self.branches[source])
        if source in self.scores:
            self.scores[target] = dict(self.scores[source])

    def drop(self, name: str):
        self.branches.pop(name, None)
        self.scores.pop(name, None)

    def sum(self, branch: str, column: str = "b") -> int:
        values = self.branches[branch] if column == "b" else self.scores[branch]
        return sum(values.values()) * self.group_size


class Backend:
    """Branch operations in the SQL of one system; times in ms per phase"""

    def __init__(self):
        self.phases = {}

    def timed(self, phase: str, fn, *args):
        start = time.perf_counter()
        result = fn(*args)
        self.phases[phase] = self.phases.get(phase, 0.0) + (time.perf_counter() - start) * 1000
        return result

    def sql(self, query: str):
        with self.conn.cursor() as cur:
            cur.execute(query)
            return cur.fetchall() if cur.description is not None else None

    def load(self, n: int, groups: int, seed: int):
        group = [i % groups for i in range(n)]
        random.Random(seed).shuffle(group)
        self.sql("CREATE TABLE t ( a INTEGER NOT NULL, b INTEGER NOT NULL, g INTEGER NOT NULL, PRIMARY KEY (a) )")
        for start in range(0, n, 10000):
            rows = ", ".join(f"({i}, 0, {group[i]})" for i in range(start, min(n, start + 10000)))
            self.sql(f"INSERT INTO t (a, b, g) VALUES {rows}")
        self.after_load()

    def after_load(self):
        pass


class TardisDB(Backend):
    name = "tardisdb"

    def __init__(self, host, port, db):
        super().__init__()
        import psycopg2
        from psycopg2.extensions import ISOLATION_LEVEL_AUTOCOMMIT
        admin = psycopg2.connect(host=host, port=port, dbname="tardisdb", user="compare", sslmode="disable")
        admin.set_isolation_level(ISOLATION_LEVEL_AUTOCOMMIT)
        with admin.cursor() as cur:
            cur.execute(f"DROP DATABASE IF EXISTS {db}")
            cur.execute(f"CREATE DATABASE {db}")
        admin.close()
        self.conn = psycopg2.connect(host=host, port=port, dbname=db, user="compare", sslmode="disable")
        self.conn.set_isolation_level(ISOLATION_LEVEL_AUTOCOMMIT)

    @staticmethod
    def _b(name):
        return "master" if name == "main" else name

    def fork(self, name, parent):
        self.timed("fork", self.sql, f"CREATE BRANCH {name} FROM {self._b(parent)}")

    def add_column(self, branch):
        self.timed("alter", self.sql, f"ALTER TABLE t VERSION {self._b(branch)} ADD COLUMN score INTEGER NOT NULL DEFAULT 0")

    def update(self, branch, group, value, score=None):
        extra = "" if score is None else f", score = {score}"
        self.timed("update", self.sql, f"UPDATE t VERSION {self._b(branch)} SET b = {value}{extra} WHERE g = {group}")

    def sum(self, branch, column="b"):
        return int(self.sql(f"SELECT sum({column}) FROM t VERSION {self._b(branch)}")[0][0] or 0)

    def sums(self, branches):
        rows = self.sql("SELECT branch, sum(b) FROM t VERSION (" + ", ".join(map(self._b, branches)) + ") GROUP BY branch")
        return {("main" if r[0] == "master" else r[0]): int(r[1]) for r in rows}

    def descendant_sums(self, root):
        rows = self.sql(f"SELECT branch, sum(b) FROM t VERSION ALL DESCENDANTS OF {self._b(root)} GROUP BY branch")
        return {("main" if r[0] == "master" else r[0]): int(r[1]) for r in rows}

    def drop(self, name, cascade=False):
        self.timed("drop", self.sql, f"DROP BRANCH {name}" + (" CASCADE" if cascade else ""))

    def merge(self, source, target):
        self.timed("merge", self.sql, f"MERGE BRANCH {source} INTO {self._b(target)}")


class Dolt(Backend):
    name = "dolt"

    def __init__(self, host, port, db):
        super().__init__()
        import pymysql
        self.conn = pymysql.connect(host=host, port=port, user="root", autocommit=True)
        self.sql(f"DROP DATABASE IF EXISTS {db}")
        self.sql(f"CREATE DATABASE {db}")
        self.sql(f"USE {db}")
        self.current = "main"
        self.parents = {}

    def after_load(self):
        self.sql("CALL DOLT_COMMIT('-Am', 'load')")

    def _checkout(self, branch):
        if self.current != branch:
            self.sql(f"CALL DOLT_CHECKOUT('{branch}')")
            self.current = branch

    def fork(self, name, parent):
        self.timed("fork", self.sql, f"CALL DOLT_BRANCH('{name}', '{parent}')")
        self.parents[name] = parent

    def add_column(self, branch):
        def alter():
            self._checkout(branch)
            self.sql("ALTER TABLE t ADD COLUMN score INTEGER NOT NULL DEFAULT 0")
        self.timed("alter", alter)
        self.timed("commit", self.sql, "CALL DOLT_COMMIT('-am', 'alter')")

    def update(self, branch, group, value, score=None):
        def write():
            self._checkout(branch)
            extra = "" if score is None else f", score = {score}"
            self.sql(f"UPDATE t SET b = {value}{extra} WHERE g = {group}")
        self.timed("update", write)
        self.timed("commit", self.sql, "CALL DOLT_COMMIT('-am', 'update')")

    def sum(self, branch, column="b"):
        return int(self.sql(f"SELECT sum({column}) FROM t AS OF '{branch}'")[0][0] or 0)

    def sums(self, branches):
        union = " UNION ALL ".join(f"SELECT '{b}', sum(b) FROM t AS OF '{b}'" for b in branches)
        return {r[0]: int(r[1] or 0) for r in self.sql(union)}

    def descendant_sums(self, root):
        names = [b for b in self.parents if self._descends(b, root)]
        return self.sums(names) if names else {}

    def _descends(self, branch, root):
        while branch in self.parents:
            branch = self.parents[branch]
            if branch == root:
                return True
        return False

    def drop(self, name, cascade=False):
        # Dolt's branches do not depend on each other: a cascade drops the descendants one by one
        victims = [name] + ([b for b in self.parents if self._descends(b, name)] if cascade else [])
        def run():
            if self.current in victims:
                self._checkout("main")
            for v in victims:
                self.sql(f"CALL DOLT_BRANCH('-D', '{v}')")
                self.parents.pop(v, None)
        self.timed("drop", run)

    def merge(self, source, target):
        def run():
            self._checkout(target)
            self.sql(f"CALL DOLT_MERGE('{source}')")
        self.timed("merge", run)


def median_ms(fn, runs):
    times = []
    for _ in range(runs):
        start = time.perf_counter()
        fn()
        times.append((time.perf_counter() - start) * 1000)
    return statistics.median(times)


def check(what, got, expected):
    if got != expected:
        raise RuntimeError(f"{what}: got {got}, expected {expected}")


def chain(backend, args):
    rng = random.Random(args.seed)
    model = Model(args.groups, args.n // args.groups)
    depths = sorted(set(int(d) for d in args.depths.split(",")))
    print("system,depth,n,leaf_sum_ms,main_sum_ms,fork_ms,update_ms,commit_ms")
    leaf, depth = "main", 0
    for target in depths:
        while depth < target:
            depth += 1
            name = f"b{depth}"
            backend.fork(name, leaf)
            model.fork(name, leaf)
            for branch in (name, "main"):
                group, value = rng.randrange(args.groups), rng.randrange(1, 1000)
                backend.update(branch, group, value)
                model.update(branch, group, value)
            leaf = name
        check(f"sum of {leaf}", backend.sum(leaf), model.sum(leaf))
        check("sum of main", backend.sum("main"), model.sum("main"))
        leaf_ms = median_ms(lambda: backend.sum(leaf), args.runs)
        main_ms = median_ms(lambda: backend.sum("main"), args.runs)
        p = backend.phases
        print(f"{backend.name},{depth},{args.n},{leaf_ms:.3f},{main_ms:.3f},{p.get('fork', 0):.1f},{p.get('update', 0):.1f},{p.get('commit', 0):.1f}", flush=True)


def fanout(backend, args):
    rng = random.Random(args.seed)
    model = Model(args.groups, args.n // args.groups)
    ks = sorted(set(int(k) for k in args.ks.split(",")))
    print("system,k,n,one_query_ms,per_branch_ms,fork_ms,update_ms,commit_ms")
    branches = []
    for k in ks:
        while len(branches) < k:
            name = f"c{len(branches) + 1}"
            backend.fork(name, "main")
            model.fork(name, "main")
            group, value = rng.randrange(args.groups), rng.randrange(1, 1000)
            backend.update(name, group, value)
            model.update(name, group, value)
            branches.append(name)
        expected = {b: model.sum(b) for b in branches}
        check(f"sums of {k} branches", backend.descendant_sums("main"), expected)
        one = median_ms(lambda: backend.descendant_sums("main"), args.runs)
        per_branch = median_ms(lambda: [backend.sum(b) for b in branches], max(1, min(args.runs, 3)))
        p = backend.phases
        print(f"{backend.name},{k},{args.n},{one:.3f},{per_branch:.3f},{p.get('fork', 0):.1f},{p.get('update', 0):.1f},{p.get('commit', 0):.1f}", flush=True)


_CREATE = re.compile(r"CREATE BRANCH (\w+) FROM (\w+);")
_ALTER = re.compile(r"ALTER TABLE t VERSION (\w+) ADD COLUMN score INTEGER(?: NOT NULL)? DEFAULT 0;")
_UPDATE = re.compile(r"UPDATE t VERSION (\w+) SET b = (\d+)(?:, score = (\d+))? WHERE g = (\d+);")
_SUM = re.compile(r"SELECT sum\((b|score)\) FROM t VERSION (\w+);")
_SUMS = re.compile(r"SELECT branch, sum\(b\) FROM t VERSION \(([\w, ]+)\) GROUP BY branch;")
_DROP = re.compile(r"DROP BRANCH (\w+)( CASCADE)?;")
_MERGE = re.compile(r"MERGE BRANCH (\w+) INTO (\w+);")


def loops(backend, args):
    """Replays a trace of branchBench --mode=agentic: the same statements, timed per phase"""
    model = Model(args.n // args.mutations, args.mutations)
    statements = [line.strip() for line in open(args.trace) if line.strip() and not line.startswith("--")]
    start = time.perf_counter()
    for s in statements:
        if m := _CREATE.fullmatch(s):
            backend.fork(m[1], m[2])
            model.fork(m[1], m[2])
        elif m := _ALTER.fullmatch(s):
            backend.add_column(m[1])
            model.add_column(m[1])
        elif m := _UPDATE.fullmatch(s):
            score = int(m[3]) if m[3] is not None else None
            backend.update(m[1], int(m[4]), int(m[2]), score)
            model.update(m[1], int(m[4]), int(m[2]), score)
        elif m := _SUM.fullmatch(s):
            check(f"sum of {m[1]} in {m[2]}", backend.timed("evaluate", backend.sum, m[2], m[1]), model.sum(m[2], m[1]))
        elif m := _SUMS.fullmatch(s):
            names = [n.strip() for n in m[1].split(",")]
            check("sums", backend.timed("evaluate", backend.sums, names), {n: model.sum(n) for n in names})
        elif m := _DROP.fullmatch(s):
            backend.drop(m[1], cascade=bool(m[2]))
            if m[2]:
                for b in [b for b in model.branches if b != "main"]:
                    model.drop(b)  # the loops drop their whole tree at the end
            model.drop(m[1])
        elif m := _MERGE.fullmatch(s):
            backend.merge(m[1], m[2])
            model.merge(m[1], m[2])
        else:
            raise RuntimeError(f"unknown statement in the trace: {s}")
    total = (time.perf_counter() - start) * 1000
    check("sum of main after the loop", backend.sum("main"), model.sum("main"))
    p = backend.phases
    print("system,trace,statements,fork_ms,update_ms,alter_ms,commit_ms,evaluate_ms,drop_ms,merge_ms,total_ms")
    print(f"{backend.name},{args.trace.rsplit('/', 1)[-1]},{len(statements)},{p.get('fork', 0):.1f},{p.get('update', 0):.1f},{p.get('alter', 0):.1f},"
          f"{p.get('commit', 0):.1f},{p.get('evaluate', 0):.1f},{p.get('drop', 0):.1f},{p.get('merge', 0):.1f},{total:.1f}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--system", choices=["tardisdb", "dolt"], required=True)
    parser.add_argument("--mode", choices=["chain", "fanout", "loops"], required=True)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int)
    parser.add_argument("--db", default="compare")
    parser.add_argument("--n", type=int, default=1000000)
    parser.add_argument("--groups", type=int, default=100, help="chain, fanout: groups (100: an update writes 1 %%)")
    parser.add_argument("--mutations", type=int, default=1000, help="loops: tuples per group, as in the trace")
    parser.add_argument("--depths", default="0,1,2,4,8,16,32,64")
    parser.add_argument("--ks", default="1,4,16,64")
    parser.add_argument("--trace")
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--seed", type=int, default=42)
    args = parser.parse_args()

    port = args.port or (55433 if args.system == "tardisdb" else 33306)
    backend = (TardisDB if args.system == "tardisdb" else Dolt)(args.host, port, args.db)
    groups = args.n // args.mutations if args.mode == "loops" else args.groups
    start = time.perf_counter()
    backend.load(args.n, groups, args.seed)
    print(f"loaded {args.n} tuples in {time.perf_counter() - start:.1f} s", file=sys.stderr)
    {"chain": chain, "fanout": fanout, "loops": loops}[args.mode](backend, args)


if __name__ == "__main__":
    main()
