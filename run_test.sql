\echo Testing :relname

checkpoint;

select setting as datadir from gp_settings where gp_segment_id = 0 and name = 'data_directory' \gset
select oid as db_oid from pg_database where datname = current_database() \gset
select relfilenode from gp_dist_random('pg_class') where gp_segment_id = 0 and relname = :'relname' \gset
\set filepath :datadir '/base/' :db_oid '/' :relfilenode :ext

\setenv filepath :filepath
\! pg_filedump $opt $filepath | sed -e "s/logid      ./logid      ./" -e "s/recoff 0x......../recoff 0x......../" -e "s/File: .*/File: /" -e "s/HeapId \(.*\)/HeapId \(\)/" -e "s/IndexId \(.*\)/IndexId \(\)/" -e "s/Checksum: ....../Checksum: /"

--
----------------------------------------------------------------------------------------------
--
