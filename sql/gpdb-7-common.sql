-- b-tree index
\! pg_filedump test/index.gp7

-- create table <name> (c1 int, c2 text)
\! pg_filedump test/heap.gp7
\! pg_filedump -d test/heap.gp7
\! pg_filedump -D int,text test/heap.gp7

-- create table <name> (c1 int, c2 text)
--   with (appendonly=true)
\! pg_filedump -z -M test/ao.gp7
\! pg_filedump -z -M -T none test/ao.gp7
\! pg_filedump -z -M -O row test/ao.gp7
\! pg_filedump -z -M -k test/ao.gp7

-- create table <name> (c1 int, c2 text)
--   with (appendonly=true, checksum=false)
\! pg_filedump -z test/ao.nochecksum.gp7

-- create table <name> (c1 int, c2 text)
--   with (appendonly=true, orientation=column)
\! pg_filedump -z -M -k -O column test/co.gp7

-- create table <name> (c1 int, c2 text)
--   with (appendonly=true, orientation=column, checksum=false)
\! pg_filedump -z -O column test/co.nochecksum.gp7
