"""Lifecycle connector for TardisDB: databases of pgserver, loaded statement by statement."""

from __future__ import annotations

import re
from contextlib import closing
from pathlib import Path

from dblib.connector import Connector, DatabaseHandle
from dblib.target import PreparedTarget, SuiteRequest

from . import adapter as tardisdb
from .adapter import TardisDBToolSuite

# Statements of a dump that TardisDB does not have and that do not change the data: foreign keys
# (not enforced), indexes of PostgreSQL's syntax, DROP TABLE on the fresh database, session settings
_SKIPPED = re.compile(r"(?is)^\s*(DROP\s+TABLE|ALTER\s+TABLE\s+\S+\s+ADD\s+(CONSTRAINT\s+\S+\s+)?FOREIGN\s+KEY|"
                      r"CREATE\s+(UNIQUE\s+)?INDEX|SET\s|SELECT\s+pg_catalog\.|\\\\)")


def _statements(sql: str) -> list:
    """The statements of a SQL file, split at semicolons outside literals, without comments"""
    out, current, quoted, i = [], [], False, 0
    while i < len(sql):
        c = sql[i]
        if not quoted and sql.startswith("--", i):
            i = sql.find("\n", i)
            i = len(sql) if i < 0 else i
            continue
        if c == "'":
            quoted = not quoted
        if c == ";" and not quoted:
            statement = "".join(current).strip()
            if statement:
                out.append(statement)
            current = []
        else:
            current.append(c)
        i += 1
    if "".join(current).strip():
        out.append("".join(current).strip())
    return out


class TardisDB(Connector):
    """TardisDB's pgserver: a database per run, created and dropped through the server."""

    environment = ("TARDISDB_",)
    needs_psql = False
    version_sql = "SHOW server_version"
    root_ref = tardisdb.MAIN

    def scope(self) -> dict:
        return {"host": tardisdb.HOST, "port": tardisdb.PORT, "user": tardisdb.USER}

    def _admin(self):
        return tardisdb.connect(tardisdb.ADMIN_DB)

    def provision(self, db: str) -> DatabaseHandle:
        with closing(self._admin()) as conn, conn.cursor() as cur:
            cur.execute(f"CREATE DATABASE {tardisdb._ident(db)}")
        return DatabaseHandle(db, self.root_ref)

    def attach(self, db: str, existing: object) -> DatabaseHandle:
        if existing.branch_id and existing.branch_id != self.root_ref:
            raise ValueError("TardisDB existing_db starts on main")
        return super().attach(db, existing)

    def load(self, database: DatabaseHandle, dump: Path) -> None:
        skipped = 0
        with closing(tardisdb.connect(database.name)) as conn, conn.cursor() as cur:
            for statement in _statements(Path(dump).read_text()):
                if _SKIPPED.match(statement):
                    skipped += 1
                    continue
                cur.execute(statement)
        if skipped:
            print(f"TardisDB: skipped {skipped} statements of {dump} (foreign keys, indexes, DROP, settings)")

    def prepare_target(self, database: DatabaseHandle) -> PreparedTarget:
        def open_suite(request: SuiteRequest):
            suite = TardisDBToolSuite.init_for_bench(request.result_collector, database.name,
                                                     request.initial_ref or self.root_ref,
                                                     measure_storage=request.measure_storage)
            if request.worker_index > 0 and request.setup_refs:
                suite.exec([], refs=[request.setup_refs[0]], timed=False)[0].raise_for_status()
            return suite

        return PreparedTarget(database.root_ref, TardisDBToolSuite, open_suite)

    def delete(self, db: str) -> None:
        with closing(self._admin()) as conn, conn.cursor() as cur:
            cur.execute(f"DROP DATABASE IF EXISTS {tardisdb._ident(db)}")
        if self.exists(db):
            raise RuntimeError(f"TardisDB database '{db}' still exists after deletion")

    def exists(self, db: str) -> bool:
        with closing(self._admin()) as conn, conn.cursor() as cur:
            cur.execute("SHOW DATABASES")
            return db in {r[0] for r in cur.fetchall()}

    def version(self) -> str:
        with closing(self._admin()) as conn, conn.cursor() as cur:
            cur.execute(self.version_sql)
            return str(cur.fetchone()[0])


CONNECTOR = TardisDB
