create table nation
  (
     n_nationkey integer not null,
     n_name      char(25) not null,
     n_regionkey integer not null,
     n_comment   varchar(152)
  );

create table region
  (
     r_regionkey integer not null,
     r_name      char(25) not null,
     r_comment   varchar(152)
  );

create table part
  (
     p_partkey     integer not null,
     p_name        varchar(55) not null,
     p_mfgr        char(25) not null,
     p_brand       char(10) not null,
     p_type        varchar(25) not null,
     p_size        integer not null,
     p_container   char(10) not null,
     p_retailprice numeric(15, 2) not null,
     p_comment     varchar(23) not null
  );

create table supplier
  (
     s_suppkey   integer not null,
     s_name      char(25) not null,
     s_address   varchar(40) not null,
     s_nationkey integer not null,
     s_phone     char(15) not null,
     s_acctbal   numeric(15, 2) not null,
     s_comment   varchar(101) not null
  );

create table partsupp
  (
     ps_partkey    integer not null,
     ps_suppkey    integer not null,
     ps_availqty   integer not null,
     ps_supplycost numeric(15, 2) not null,
     ps_comment    varchar(199) not null
  );

create table customer
  (
     c_custkey    integer not null,
     c_name       varchar(25) not null,
     c_address    varchar(40) not null,
     c_nationkey  integer not null,
     c_phone      char(15) not null,
     c_acctbal    numeric(15, 2) not null,
     c_mktsegment char(10) not null,
     c_comment    varchar(117) not null
  );

create table orders
  (
     o_orderkey      integer not null,
     o_custkey       integer not null,
     o_orderstatus   char(1) not null,
     o_totalprice    numeric(15, 2) not null,
     o_orderdate     date not null,
     o_orderpriority char(15) not null,
     o_clerk         char(15) not null,
     o_shippriority  integer not null,
     o_comment       varchar(79) not null
  );

create table lineitem
  (
     l_orderkey      integer not null,
     l_partkey       integer not null,
     l_suppkey       integer not null,
     l_linenumber    integer,
     l_quantity      numeric(15, 2),
     l_extendedprice numeric(15, 2),
     l_discount      numeric(15, 2),
     l_tax           numeric(15, 2),
     l_returnflag    char(1) not null,
     l_linestatus    char(1) not null,
     l_shipdate      date not null,
     l_commitdate    date not null,
     l_receiptdate   date not null,
     l_shipinstruct  char(25) not null,
     l_shipmode      char(10) not null,
     l_comment       varchar(44) not null
  );
