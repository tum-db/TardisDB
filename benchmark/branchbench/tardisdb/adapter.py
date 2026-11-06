"""TardisDB backend: branches of TardisDB through its PostgreSQL-compatible server (pgserver).

TardisDB keeps the branches of a database in one MVCC store: a fork is a metadata record with an
interval label, a branch reads its versions through the version chains, and the compiled scan tests
visibility inline. A session of pgserver has a current branch, in which the statements without
VERSION run:

    branch     CREATE BRANCH <name> FROM <parent> (native); only from a branch head
    connect    SET branch = <name>
    merge      MERGE BRANCH <source> INTO <target> ON CONFLICT KEEP TARGET | KEEP SOURCE (native, per
               attribute against the newest version both had when they split): on_conflict "ours" keeps
               the target, "theirs" the source; a resolver callable is not available (no pending merge
               state to resolve in)
    diff       SELECT * FROM DIFF(t VERSION a, t VERSION b) per table (native: only the tuples
               written since the branches split), counted by primary key
    delete     DROP BRANCH <name> (native); a branch with descendants cannot be dropped
    multi-ref  db.table(ref, t) is "t VERSION ref": one statement reads several branches
    exec
    commit, log, rebase, revert, reset: unsupported (TardisDB has branches, but no commits)

TardisDB has no information_schema: the connection carries a catalog (SHOW TABLES, SHOW COLUMNS FROM
t) that util.db_helpers reads instead. Versions hold no null indicators: columns declared nullable take
no NULL values. FLOAT, REAL and DOUBLE are numeric(18, 6).

Environment: TARDISDB_HOST (127.0.0.1), TARDISDB_PORT (55433), TARDISDB_USER (branchbench).
"""

import os
import re

import psycopg2
import psycopg2.extensions
from psycopg2.extensions import ISOLATION_LEVEL_AUTOCOMMIT

from dblib.db_api import DBToolSuite, Ref
from dblib.pg_merge import empty_diff
import dblib.result_collector as rc

HOST = os.environ.get("TARDISDB_HOST", "127.0.0.1")
PORT = int(os.environ.get("TARDISDB_PORT", "55433"))
USER = os.environ.get("TARDISDB_USER", "branchbench")
ADMIN_DB = "tardisdb"
MAIN = "main"

_NAME = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def connection_uri(db: str) -> str:
    return f"host={HOST} port={PORT} dbname={db} user={USER} sslmode=disable"


def _ident(name: str) -> str:
    """TardisDB takes plain identifiers only (no quoting)"""
    if not _NAME.fullmatch(name):
        raise ValueError(f"TardisDB cannot name {name!r}: identifiers are letters, digits and _")
    return name


class _Catalog:
    """The tables of the session's branch, read by util.db_helpers in place of information_schema."""

    def __init__(self, conn):
        self._conn = conn

    def _rows(self, query: str) -> list:
        with self._conn.cursor() as cur:
            cur.execute(query)
            return cur.fetchall()

    def tables(self) -> list:
        return [r[0] for r in self._rows("SHOW TABLES")]

    def described(self, table: str) -> list:
        """(column, data type, nullable, position in the primary key or 0) per column"""
        return self._rows(f"SHOW COLUMNS FROM {_ident(table.split('.')[-1])}")

    def columns(self, table: str) -> list:
        return [r[0] for r in self.described(table)]

    def pk_columns(self, table: str) -> list:
        keyed = sorted((int(r[3]), r[0]) for r in self.described(table) if int(r[3]) > 0)
        return [name for _, name in keyed]


class _Connection(psycopg2.extensions.connection):
    """A psycopg2 connection that carries the catalog"""

    catalog = None


def connect(db: str):
    conn = psycopg2.connect(connection_uri(db), connection_factory=_Connection)
    conn.set_isolation_level(ISOLATION_LEVEL_AUTOCOMMIT)
    conn.catalog = _Catalog(conn)
    return conn


class TardisDBToolSuite(DBToolSuite):
    BACKEND_NAME = "tardisdb"
    # pgserver holds all databases in one process; storage is its resident memory
    STORAGE_SCOPE = "server"
    SUPPORTS_COMMIT_REFS = False
    SUPPORTS_MULTI_REF_EXEC = True
    IMPLEMENTATION_NOTES = {
        "branch": "CREATE BRANCH: a metadata record with an interval label; no data is copied",
        "merge": "MERGE BRANCH ... ON CONFLICT KEEP TARGET | KEEP SOURCE: three-way per attribute",
        "diff": "DIFF over the tuples written since the branches split, counted by primary key",
        "delete": "DROP BRANCH frees the versions no remaining branch reads",
    }

    @classmethod
    def init_for_bench(cls, collector: rc.ResultCollector, db_name: str, initial_ref: str,
                       measure_storage: bool = False):
        suite = cls(connect(db_name), collector, db_name, measure_storage)
        suite._connect_impl(Ref.parse(initial_ref or MAIN))
        suite._current_ref = Ref.parse(initial_ref or MAIN)
        return suite

    def __init__(self, connection, collector: rc.ResultCollector, db_name: str, measure_storage: bool = False):
        super().__init__(connection, collector, measure_storage)
        self.db_name = db_name

    # -- helpers ---------------------------------------------------------------

    def _run(self, query: str):
        with self.conn.cursor() as cur:
            cur.execute(query)
            return cur.fetchall() if cur.description is not None else None

    @staticmethod
    def _branch_name(ref: Ref) -> str:
        if ref.commit:
            raise ValueError("TardisDB has no commits")
        return _ident(ref.branch)

    def list_branches(self) -> list:
        return [MAIN if r[0] == "master" else r[0] for r in self._run("SHOW BRANCHES")]

    def get_table_schema(self, table_name: str) -> str:
        """CREATE TABLE of the session's branch, with the types of PostgreSQL"""
        columns = self.conn.catalog.described(table_name)
        if not columns:
            raise Exception(f"Error: Table '{table_name}' not found.")
        definitions = [f"  {name} {data_type} NOT NULL" for name, data_type, _, _ in columns]
        return "CREATE TABLE {} (\n{}\n);".format(table_name, ",\n".join(definitions))

    def _get_table_columns(self, table_name: str) -> list:
        out = []
        for name, data_type, _, _ in self.conn.catalog.described(table_name):
            m = re.fullmatch(r"([a-z ]+?)(?:\((\d+)(?:,(\d+))?\))?", data_type)
            base, n, scale = m.group(1), m.group(2), m.group(3)
            length = int(n) if n and base.startswith("character") else None
            precision = int(n) if n and base == "numeric" else None
            out.append((name, base, "NO", length, precision, int(scale) if scale else None))
        return out

    def _qualified_table(self, ref: Ref, table: str) -> str:
        return f"{_ident(table)} VERSION {self._branch_name(ref)}"

    # -- hooks -----------------------------------------------------------------

    def _connect_impl(self, ref: Ref) -> None:
        self._run(f"SET branch = {self._branch_name(ref)}")

    def _branch_impl(self, name: str, from_ref: Ref) -> None:
        if from_ref.commit:
            raise self._unsupported("branch", "TardisDB forks only from a branch head (no commits)")
        self._run(f"CREATE BRANCH {_ident(name)} FROM {self._branch_name(from_ref)}")

    def _delete_impl(self, ref: Ref) -> None:
        self._run(f"DROP BRANCH {self._branch_name(ref)}")
        if self._current_ref and self._current_ref.branch == ref.branch:
            self._current_ref = None

    def _merge_impl(self, into: Ref, source: Ref, message: str, on_conflict="ours") -> dict:
        if callable(on_conflict):
            raise self._unsupported("merge", "TardisDB resolves conflicts by a policy (ours, theirs), not by a callable")
        side = {"ours": "TARGET", "theirs": "SOURCE"}.get(on_conflict)
        if side is None:
            raise ValueError(f"unknown on_conflict {on_conflict!r}")
        del self.conn.notices[:]
        self._run(f"MERGE BRANCH {self._branch_name(source)} INTO {self._branch_name(into)} ON CONFLICT KEEP {side}")
        # pgserver reports the resolved conflicts as a notice: "<n> conflict(s) kept the target"
        resolved = sum(int(m.group(1)) for n in self.conn.notices if (m := re.search(r"(\d+) conflicts? kept", n)))
        return {"fast_forward": False, "conflicts": resolved}

    def _diff_impl(self, ref_a: Ref, ref_b: Ref) -> dict:
        a, b = self._branch_name(ref_a), self._branch_name(ref_b)
        if a == b:
            return empty_diff()
        tables = []
        for table in self.conn.catalog.tables():
            key = self.conn.catalog.pk_columns(table)
            names = self.conn.catalog.columns(table)
            rows = self._run(f"SELECT * FROM DIFF({_ident(table)} VERSION {a}, {_ident(table)} VERSION {b})") or []
            # each row: the branch that sees the version, then the columns
            positions = [names.index(k) + 1 for k in key] if key else list(range(1, len(names) + 1))
            sides: dict = {}
            for row in rows:
                branch = MAIN if row[0] == "master" and MAIN in (a, b) else row[0]
                sides.setdefault(tuple(row[p] for p in positions), set()).add(branch)
            added = sum(1 for s in sides.values() if s == {b})
            deleted = sum(1 for s in sides.values() if s == {a})
            modified = sum(1 for s in sides.values() if s == {a, b})
            if added or deleted or modified:
                tables.append({"table_name": table, "rows_added": added, "rows_deleted": deleted, "rows_modified": modified})
        return {"tables": tables, "rows_added": sum(t["rows_added"] for t in tables),
                "rows_deleted": sum(t["rows_deleted"] for t in tables),
                "rows_modified": sum(t["rows_modified"] for t in tables)}

    def _storage_bytes(self) -> int:
        """Resident memory of pgserver (all databases), from its pid file"""
        state = os.environ.get("DBFORK_STATE_DIR", "")
        try:
            pid = int(open(os.path.join(state, "tardisdb.pid")).read().strip())
            with open(f"/proc/{pid}/status") as status:
                for line in status:
                    if line.startswith("VmRSS:"):
                        return int(line.split()[1]) * 1024
        except (OSError, ValueError):
            pass
        return 0

    # -- async (exec_async) ------------------------------------------------------

    async def open_async_pool(self, size: int) -> None:
        from psycopg_pool import AsyncConnectionPool

        if self._current_ref is None:
            raise ValueError("Not connected to a branch")
        self._pool_branch = self._current_ref.branch
        self.async_pool = AsyncConnectionPool(
            connection_uri(self.db_name) + f" options='-c branch={self._branch_name(self._current_ref)}'",
            min_size=size, max_size=size, kwargs={"autocommit": True}, open=False)
        await self.async_pool.open(wait=True)

    async def _connect_impl_async(self, conn, ref: Ref):
        await conn.execute(f"SET branch = {self._branch_name(ref)}")
        return conn
