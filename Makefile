# only support building in pgxs mode
USE_PGXS = 1

GPDB_REPO ?= https://raw.githubusercontent.com/greenplum-db/gpdb
top_builddir ?= $(HOME)/src/gpdb-$(GPDB_RELEASE).git

# the pgxs makefiles provide some version variables like below:
#
#     MAJORVERSION = 12
#     GP_MAJORVERSION = 7
#
# however we need them before including the makefiles, so we have to guess them
# manually with pg_config
PG_CONFIG_MAJOR_VERSION = \
	$(shell pg_config --version \
	  | egrep --only-matching '[0-9]+(\.[0-9]+)?' \
	  | head -n1)

ifeq ($(PG_CONFIG_MAJOR_VERSION),)
$(error cannot detect gpdb version with pg_config)
endif # PG_CONFIG_MAJOR_VERSION

ifeq ($(PG_CONFIG_MAJOR_VERSION),12.12)
	# gpdb-7, postgres-12 based
	GPDB_RELEASE = 7
	GPDB_COMMIT = 0a7a3566873325aca1789ae6f818c80f17a9402d
	# pg_control has incompatible formats between gpdb major versions
	REGRESS += gpdb-7_12.12-control
endif # PG_CONFIG_MAJOR_VERSION

ifeq ($(GPDB_COMMIT),)
$(error unsupported gpdb version: $(shell pg_config --version))
endif # GPDB_COMMIT

PROGRAM = pg_filedump
OBJS	= decode.o pg_filedump.o stringinfo.o

DOCS = README.pg
DOCS += README.md

PKG_CONFIG ?= pkg-config --silence-errors

# auto detect zlib if not explicitly specified
ifndef ENABLE_ZLIB
	ENABLE_ZLIB = $(shell $(PKG_CONFIG) --exists zlib && echo y || echo n)
endif # ENABLE_ZLIB
# try to enable zlib
ifeq ($(ENABLE_ZLIB),y)
	ifeq ($(shell $(PKG_CONFIG) --libs zlib),)
$(error cannot find zlib)
	endif

	PG_CPPFLAGS += -D ENABLE_ZLIB
	PG_CPPFLAGS += $(shell $(PKG_CONFIG) --cflags zlib)
	PG_LIBS += $(shell $(PKG_CONFIG) --libs zlib)
endif # ENABLE_ZLIB

# auto detect libzstd if not explicitly specified
ifndef ENABLE_ZSTD
	ENABLE_ZSTD = $(shell $(PKG_CONFIG) --exists libzstd && echo y || echo n)
endif # ENABLE_ZSTD
# try to enable libzstd
ifeq ($(ENABLE_ZSTD),y)
	ifeq ($(shell $(PKG_CONFIG) --libs libzstd),)
$(error cannot find libzstd)
	endif

	PG_CPPFLAGS += -D ENABLE_ZSTD
	PG_CPPFLAGS += $(shell $(PKG_CONFIG) --cflags libzstd)
	PG_LIBS += $(shell $(PKG_CONFIG) --libs libzstd)
endif # ENABLE_ZSTD

$(info ENABLE_ZLIB: $(ENABLE_ZLIB))
$(info ENABLE_ZSTD: $(ENABLE_ZSTD))

OBJS += gpdb.o
OBJS += mock.o

REGRESS += gpdb-$(GPDB_RELEASE)-common

ifeq ($(ENABLE_ZLIB),y)
	REGRESS += gpdb-$(GPDB_RELEASE)-zlib
endif # ENABLE_ZLIB

ifeq ($(ENABLE_ZSTD),y)
ifeq ($(filter $(GPDB_RELEASE),6 7),)
	REGRESS += gpdb-$(GPDB_RELEASE)-zstd
endif
endif # ENABLE_ZSTD

ifdef USE_PGXS
	PG_CONFIG = pg_config
	PGXS := $(shell $(PG_CONFIG) --pgxs)
	include $(PGXS)
endif

# avoid linking against all libs that the server links against (xml, selinux, ...)
LIBS = $(libpq_pgport)

# add init_file
REGRESS_OPTS = --init-file=$(srcdir)/init_file

# always redownload all the files
.PHONY: download
download:
	$(MAKE) -f $(firstword $(MAKEFILE_LIST))
