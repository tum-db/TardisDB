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
c.close()
c2.close()
with admin.cursor() as cur:
    cur.execute("DROP DATABASE drivers")
print("OK")
