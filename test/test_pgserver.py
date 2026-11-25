"""Clients against pgserver: psycopg2 (simple protocol, client-side parameters) and psycopg 3 (extended
protocol, binary parameters). Usage: python3 test_pgserver.py [port]; pgserver must listen on 127.0.0.1."""
import datetime, decimal, sys
import psycopg2, psycopg
from psycopg2.extensions import ISOLATION_LEVEL_AUTOCOMMIT

port = int(sys.argv[1]) if len(sys.argv) > 1 else 55433
admin = psycopg2.connect(f"host=127.0.0.1 port={port} dbname=tardisdb user=test sslmode=disable")
admin.set_isolation_level(ISOLATION_LEVEL_AUTOCOMMIT)
with admin.cursor() as cur:
    cur.execute("DROP DATABASE IF EXISTS drivers")
    cur.execute("CREATE DATABASE drivers")
dsn = f"host=127.0.0.1 port={port} dbname=drivers user=test sslmode=disable"

c = psycopg2.connect(dsn)
c.set_isolation_level(ISOLATION_LEVEL_AUTOCOMMIT)
cur = c.cursor()
cur.execute("create table k ( id integer not null, name varchar(20) not null, amount numeric(10,2) not null, at timestamp not null )")
for i in range(1, 6):
    cur.execute("insert into k (id, name, amount, at) values (%s, %s, %s, %s)",
                (i, f"n'{i}", decimal.Decimal(f"{i}.25"), datetime.datetime(2025, 1, i, 12, 0, 0)))
    assert cur.rowcount == 1, cur.rowcount
cur.execute("select id, name, amount, at from k where id = %s", (3,))
row = cur.fetchone()
print("psycopg2 row:", row, [d.name for d in cur.description])
assert row == (3, "n'3", decimal.Decimal("3.25"), datetime.datetime(2025, 1, 3, 12, 0)), row
cur.execute("update k set amount = %s where id = %s", (decimal.Decimal("9.99"), 2))
assert cur.rowcount == 1
cur.execute("select count(*), sum(amount) from k")
print("psycopg2 aggregate:", cur.fetchone())
cur.execute("create branch dev from master")
cur.execute("set branch = dev")
cur.execute("delete from k where id = %s", (1,))
print("psycopg2 delete rowcount:", cur.rowcount)
cur.execute("select count(*) from k")
assert cur.fetchone()[0] == 4
cur.execute("set branch = master")
cur.execute("select count(*) from k")
assert cur.fetchone()[0] == 5
try:
    cur.execute("select nope from k")
except psycopg2.Error as e:
    print("psycopg2 error:", e.pgcode, str(e).strip())
cur.execute("select id from k where id = 4")
assert cur.fetchall() == [(4,)]

# without autocommit: psycopg2 sends BEGIN before the first statement, and the block is a transaction
c2 = psycopg2.connect(dsn)
cur2 = c2.cursor()
cur2.execute("select count(*) from k")
print("psycopg2 in a block:", cur2.fetchone())
c2.commit()
cur2.execute("insert into k (id, name, amount, at) values (6, 'six', 6.00, '2025-01-06 00:00:00')")
cur.execute("select count(*) from k")
assert cur.fetchone()[0] == 5  # not before the commit
c2.rollback()
cur.execute("select count(*) from k")
assert cur.fetchone()[0] == 5
# of two writes of a tuple, the second to commit fails
cur2.execute("update k set amount = 1.00 where id = 5")
cur.execute("update k set amount = 2.00 where id = 5")
try:
    c2.commit()
    raise AssertionError("the second writer of a tuple must not commit")
except psycopg2.Error as e:
    assert e.pgcode == "40001", e.pgcode
    print("psycopg2 commit after a concurrent update:", e.pgcode)
    c2.rollback()
cur2.execute("insert into k (id, name, amount, at) values (6, 'six', 6.00, '2025-01-06 00:00:00')")
c2.commit()
cur.execute("select count(*) from k")
assert cur.fetchone()[0] == 6
cur.execute("select amount from k where id = 5")
assert cur.fetchone()[0] == decimal.Decimal("2.00")
cur.execute("delete from k where id = 6")

# psycopg 3: extended protocol with server-side parameters
with psycopg.connect(dsn + " options='-c branch=dev'", autocommit=True) as c3:
    rows = c3.execute("select id, name from k where id = %s", (2,)).fetchall()
    print("psycopg3 on dev:", rows)
    assert rows == [(2, "n'2")]
    cur3 = c3.execute("update k set name = %s where id = %s", ("two", 2))
    print("psycopg3 update rowcount:", cur3.rowcount)
    print("psycopg3 count:", c3.execute("select count(*) from k").fetchone())
    try:
        c3.execute("select nope from k")
    except psycopg.Error as e:
        print("psycopg3 error:", e.sqlstate)
    print("psycopg3 after error:", c3.execute("select count(*) from k").fetchone())
# a conflict resolved by a policy: the tag counts the changed tuples, a notice the resolved conflicts
cur.execute("create branch m1 from master")
cur.execute("update k version m1 set name = 'left' where id = 3")
cur.execute("update k set name = 'right' where id = 3")
try:
    cur.execute("merge branch m1 into master")
    raise AssertionError("a conflicting merge without a policy must fail")
except psycopg2.Error:
    pass
del c.notices[:]
cur.execute("merge branch m1 into master on conflict keep source")
print("merge notices:", [n.strip() for n in c.notices], "rowcount", cur.rowcount)
assert any("1 conflict kept the source" in n for n in c.notices)
cur.execute("select name from k where id = 3")
assert cur.fetchone()[0] == "left"
# NULL values: None as a parameter of both drivers, NULL in results, and NOT NULL
cur.execute("create table nn ( id integer not null, v integer, s varchar(10) )")
cur.execute("insert into nn (id, v, s) values (%s, %s, %s), (%s, %s, %s)", (1, None, "a", 2, 20, None))
cur.execute("select id, v, s from nn where v is null")
assert cur.fetchall() == [(1, None, "a")]
cur.execute("select count(*), count(v), sum(v) from nn")
assert cur.fetchone() == (2, 1, 20)
with psycopg.connect(dsn, autocommit=True) as c3:
    c3.execute("update nn set s = %s where id = %s", (None, 1))
    assert c3.execute("select id, s from nn where s is null").fetchall() == [(1, None), (2, None)]
try:
    cur.execute("insert into nn (id, v) values (%s, %s)", (None, 1))
    raise AssertionError("NULL in a NOT NULL column")
except psycopg2.Error as e:
    assert e.pgcode == "23502", e.pgcode
    print("psycopg2 NULL in a NOT NULL column:", e.pgcode)
# joins: table names as bindings, parameters of both drivers
cur.execute("insert into nn (id, v, s) values (3, 30, 'c'), (3, 31, 'cc')")
cur.execute("select k.name, nn.s from k, nn where k.id = nn.id and nn.v > %s", (25,))
rows = sorted(cur.fetchall())
print("psycopg2 join:", rows)
assert rows == [("left", "c"), ("left", "cc")], rows
with psycopg.connect(dsn, autocommit=True) as c3:
    rows = c3.execute("select k.id, v from k, nn where k.id = nn.id and k.id = %s", (2,)).fetchall()
    assert rows == [(2, 20)], rows
# arrays and doubles: Python lists of both drivers (psycopg2 sends ARRAY[...], psycopg 3 typed
# parameters), arrays and floats in results, array functions and operators
cur.execute("create table ar ( id integer not null, v integer[], s text[], d double precision, m double precision[] )")
cur.execute("insert into ar (id, v, s, d, m) values (%s, %s, %s, %s, %s)", (1, [1, 2, 3], ["a b", None, "c"], 0.1, [[1.5, 2], [3, 4]]))
cur.execute("select id, v, s, d, m from ar")
row = cur.fetchone()
print("psycopg2 arrays:", row)
assert row == (1, [1, 2, 3], ["a b", None, "c"], 0.1, [[1.5, 2.0], [3.0, 4.0]]), row
cur.execute("select v[2], v[2:3], cardinality(v), array_append(v, %s), m[2][1] from ar where %s = any(v)", (9, 3))
assert cur.fetchone() == (2, [2, 3], 3, [1, 2, 3, 9], 3.0)
with psycopg.connect(dsn, autocommit=True) as c3:
    c3.execute("insert into ar (id, v, s, d) values (%s, %s, %s, %s)", (2, [4, None], ["x"], 1e20))
    assert c3.execute("select id, v, s, d from ar where id = %s", (2,)).fetchone() == (2, [4, None], ["x"], 1e20)
    assert c3.execute("select id from ar where v @> %s", ([2],)).fetchall() == [(1,)]
    c3.execute("update ar set v = v || %s where id = %s", ([5, 6], 2))
    assert c3.execute("select v, array_to_string(v, ',', '*') from ar where id = 2").fetchone() == ([4, None, 5, 6], "4,*,5,6")
    assert c3.execute("select sum(d), avg(d) from ar").fetchone() == (1e20 + 0.1, (1e20 + 0.1) / 2)
    # parameters in binary: arrays (with NULLs and two dimensions) and doubles
    c3.execute("insert into ar (id, v, s, d, m) values (%b, %b, %b, %b, %b)", (3, [7, None, 9], ["q", None], 2.5, [[1.0, 2.0], [3.0, 4.5]]))
    assert c3.execute("select v, s, d, m from ar where id = 3").fetchone() == ([7, None, 9], ["q", None], 2.5, [[1.0, 2.0], [3.0, 4.5]])
# a join with expressions over arrays
cur.execute("select k.name, ar.v[1] + nn.v from k, ar, nn where k.id = ar.id and ar.id = nn.id and ar.id = 2")
assert cur.fetchall() == [("n'2", 24)]
# WITH RECURSIVE, UNION, INTERSECT, EXCEPT and SELECT without FROM, with parameters of both drivers
cur.execute("with recursive t(n) as (select 1 union all select n + 1 from t where n < %s) select n, n * n as sq from t", (4,))
assert [d.name for d in cur.description] == ["n", "sq"], cur.description
assert sorted(cur.fetchall()) == [(1, 1), (2, 4), (3, 9), (4, 16)]
assert cur.rowcount == 4
cur.execute("select 1 union select 2.5")
assert sorted(r[0] for r in cur.fetchall()) == [decimal.Decimal("1.0"), decimal.Decimal("2.5")]
cur.execute("select 1")
assert cur.fetchall() == [(1,)]
cur.execute("select id from k where id < %s intersect all select id from k where id > %s", (4, 1))
assert sorted(r[0] for r in cur.fetchall()) == [2, 3]
cur.execute("select id from k except select id from k where id <> 2")
assert cur.fetchall() == [(2,)]
with psycopg.connect(dsn, autocommit=True) as c3:
    rows = c3.execute("with recursive up(id, level) as (select id, 0 from k where id = %s "
                      "union all select k.id, up.level + 1 from k, up where k.id = up.id - 1) select id, level from up", (3,)).fetchall()
    assert sorted(rows) == [(1, 2), (2, 1), (3, 0)], rows
    print("psycopg3 recursive:", sorted(rows))
# linear algebra on arrays: matrix product of lists of both drivers (psycopg 3 in binary)
cur.execute("select %s::double precision[] ** %s::double precision[], sig(0)", ([[1, 2], [3, 4]], [[5, 6], [7, 8]]))
assert cur.fetchone() == ([[19.0, 22.0], [43.0, 50.0]], 0.5)
with psycopg.connect(dsn, autocommit=True) as c3:
    row = c3.execute("select transpose(%b::double precision[]) ** %b::double precision[]", ([[1.0, 2.0]], [[3.0, 4.0]])).fetchone()
    assert row == ([[3.0, 4.0], [6.0, 8.0]],), row
# sum and avg of arrays, element by element, per group
cur.execute("create table ag ( g integer not null, a double precision[] )")
cur.execute("insert into ag (g, a) values (%s, %s), (%s, %s), (%s, %s)", (1, [1.0, 2.0], 1, [3.0, 4.0], 2, [5.0, 6.0]))
cur.execute("select g, sum(a), avg(a) from ag group by g")
assert sorted(cur.fetchall()) == [(1, [4.0, 6.0], [2.0, 3.0]), (2, [5.0, 6.0], [5.0, 6.0])]
with psycopg.connect(dsn, autocommit=True) as c3:
    assert c3.execute("select avg(a * %b::double precision[]) from ag", ([2.0, 0.5],)).fetchone() == ([6.0, 2.0],)
# HAVING with a parameter
cur.execute("select g, count(*) from ag group by g having count(*) > %s", (1,))
assert cur.fetchall() == [(1, 2)]
# EXPLAIN ANALYZE: one row of valid JSON with the row counts, also with a parameter of psycopg 3
import json
cur.execute("explain analyze select g from ag where g > %s", (1,))
plan = json.loads(cur.fetchone()[0])
assert plan[0]["Plan"]["Node Type"] == "Result" and plan[0]["Plan"]["Actual Rows"] == 1, plan
assert "Execution Time" in plan[0] and cur.description[0].name == "QUERY PLAN"
with psycopg.connect(dsn, autocommit=True) as c3:
    plan = json.loads(c3.execute("explain (analyze, format json) select g, count(*) from ag where g >= %s group by g", (1,)).fetchone()[0])
    assert [q["Query"] for q in plan[0]["Plan"]["Plans"]] == ["aggregate rows", "aggregate"], plan
cur.execute("show columns from ar")
types = {r[0]: r[1] for r in cur.fetchall()}
assert types["v"] == "integer[]" and types["d"] == "double precision", types
# ORDER BY, with a parameter in both drivers (psycopg 3 describes the statement before it runs)
cur.execute("select id, name from k where id > %s order by name desc", (2,))
assert cur.fetchall() == [(5, "n'5"), (4, "n'4"), (3, "left")] and [d.name for d in cur.description] == ["id", "name"]
with psycopg.connect(dsn, autocommit=True) as c3:
    assert c3.execute("select id from k where id < %s order by amount desc, id", (4,)).fetchall() == [(2,), (3,), (1,)]
# the sort-merge join of the session (SET enable_hashjoin = off), with the rows of the hash join
cur.execute("select a.id, b.id from k a, k b where a.id = b.id - 1")
hashed = sorted(cur.fetchall())
cur.execute("set enable_hashjoin = off")
cur.execute("show enable_hashjoin")
assert cur.fetchone() == ("off",)
cur.execute("explain select a.id, b.id from k a, k b where a.id = b.id - 1")
assert "Merge Join" in cur.fetchone()[0]
cur.execute("select a.id, b.id from k a, k b where a.id = b.id - 1")
assert sorted(cur.fetchall()) == hashed == [(1, 2), (2, 3), (3, 4), (4, 5)], hashed
cur.execute("set enable_hashjoin = on")
# psql's \d, \dt, \d table, \l and \dn query pg_catalog; pgserver answers them (if psql is installed)
import shutil, subprocess
if shutil.which("psql"):
    def psql(command, branch="master"):
        done = subprocess.run(["psql", "-X", "-A", "-t", "-h", "127.0.0.1", "-p", str(port), "-U", "test", "-d", "drivers",
                               "-c", f"set branch = {branch}", "-c", command],
                              capture_output=True, text=True, timeout=30, env={"PGCONNECT_TIMEOUT": "10", "PATH": "/usr/bin:/bin"})
        return done.stdout.splitlines()[1:], done.stderr.strip()
    rows, _ = psql("\\dt")
    assert "public|k|table|test" in rows and "public|ag|table|test" in rows, rows
    assert psql("\\dt a*")[0] == ["public|ag|table|test", "public|ar|table|test"], psql("\\dt a*")
    assert psql("\\d k")[0] == ["id|integer||not null|", "name|character varying(20)||not null|",
                                 "amount|numeric(10,2)||not null|", "at|timestamp without time zone||not null|"], psql("\\d k")
    cur.execute("alter table ag version dev add column extra integer")
    assert psql("\\d ag", "dev")[0][-1] == "extra|integer|||" and len(psql("\\d ag")[0]) == 2  # the columns of the branch
    assert psql("\\d nosuch")[1] == 'Did not find any relation named "nosuch".'
    assert "drivers|test|UTF8|libc|C|C|||" in psql("\\l")[0]
    assert psql("\\dn")[0] == ["public|test"]
    assert "TardisDB has no pg_catalog" in psql("\\df")[1]
    print("psql: \\d, \\dt, \\l and \\dn")
c.close()
c2.close()
with admin.cursor() as cur:
    cur.execute("DROP DATABASE drivers")
print("OK")
