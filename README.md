# Greenplum Database Filedump 

Greenplum Database Filedump is a utility to format Greenplum heap/index/control
files into a human-readable form. You can format/dump the files several ways, 
as listed in the Invocation section, as well as dumping straight binary. 

This project is based on [PostgreSQL Filedump](https://git.postgresql.org/gitweb/?p=pg_filedump.git;a=summary), 
with modification for support of Greenplum Database. Refer to 
the [PostgreSQL Filedump wiki](https://wiki.postgresql.org/wiki/Pg_filedump) 
for more information. 

The original readme of PostgreSQL Filedump is included as [README.pg_filedump](README.pg_filedump).

Build
-----

This package only supports building in pgxs mode, which means you will need to 
have a properly configured install tree (with include files) of the appropriate 
GPDB major version.

You can typically build like this:

    # make sure we can find the gpdb package
    source /path/to/greenplum_path.sh

    # build the pg_filedump
    make

The build result is a binary file named `pg_filedump`, you could copy it to
your target machines and run it without the gpdb binaries.

Gpdb versions
-------------

This package only supports gpdb 7 and onward. For the use with past gpdb major 
versions, checkout branch `historical` and follow `README.gpdb.md` there.

Configuration
-------------

There are several configurable options:

- `ENABLE_ZLIB`: To use `pg_filedump` on zlib compressed data files, we need to
  enable the zlib support.  It is automatically enabled if the zlib headers and
  libraries are detected with `pkg-config`, but you could force it to be
  enabled by `make -f Makefile.gpdb ENABLE_ZLIB=y`, make sure the zlib headers
  and libraries are available, you may also need to set `CFLAGS` and `LDFLAGS`
  accordingly.  It is also possible to force building without zlib by setting
  this variable to `n`.

- `ENABLE_ZSTD`: A similar setting for the zstd compression method. 
