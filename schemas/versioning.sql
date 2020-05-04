-- Versiontable
-- vid tableid rlist
create table versiontable (
   vid integer not null,
   tableid varchar(32) not null,
   rid integer not null
);

-- Metatable
-- vid parent message
create table metatable (
    vid integer not null,
    parent integer not null,
    message varchar(32) not null
);

-- Usertable
-- rid user_id user_name
create table users (
    rid integer not null,
    user_id integer not null,
    user_name varchar(32) not null
);

-- Tasktable
-- rid user_id task_name
create table tasks (
    rid integer not null,
    user_id integer not null,
    task_name varchar(32) not null
);
