# contrib/json_fdw/Makefile

MODULE_big = json_fdw

OBJS = json_fdw.o curlapi.o regexapi.o gettickcount.o

ifeq ($(shell uname -s), Linux)
    # Directly link against yajl 2, so it works in Ubuntu 12.04 too.
    SHLIB_LINK = -lz -l:libyajl.so.2
else
    # Non-linux OS's (in particular, OS X) don't support "-l:" syntax, 
    # so use the -lyajl flag instead.
    SHLIB_LINK = -lz -lyajl
endif

EXTENSION = json_fdw
DATA = json_fdw--1.0.sql

REGRESS = basic_tests customer_reviews hdfs_block invalid_gz_file
EXTRA_CLEAN = sql/basic_tests.sql expected/basic_tests.out \
              sql/customer_reviews.sql expected/customer_reviews.out \
              sql/hdfs_block.sql expected/hdfs_block.out \
              sql/invalid_gz_file.sql expected/invalid_gz_file.out

#
# Users need to specify their Postgres installation path through pg_config. For
# example: /usr/local/pgsql/bin/pg_config or /usr/lib/postgresql/9.2/bin/pg_config
#

# find pg_config
OS:=$(shell uname -s)
PG_CONFIG:= $(shell which pg_config)
FIND_ROOTPATH:= "/"
ifeq (${OS},Darwin)
FIND_ROOTPATH:= $(shell if [ -d "/Applications" ]; then echo "/Applications"; else echo "/"; fi)
endif
PG_CONFIG:= $(shell if [ ! -e "pg_config.loc" ]; then find $(FIND_ROOTPATH) -name pg_config > pg_config.loc; fi; cat pg_config.loc)

# for localy built uinstalled libraries, do this
YAJLDIR= ../yajl.git/build/yajl-2.1.1
PG_CPPFLAGS+= -I$(YAJLDIR)/include
SHLIB_LINK+= -L$(YAJLDIR)/lib

ZLIBDIR= ../zlib-1.2.8
PG_CPPFLAGS+= -I$(ZLIBDIR)
SHLIB_LINK+= -L$(ZLIBDIR)

# for localy build uninstalled curl, do this
CURLDIR= ../curl-7.40.0
PG_CPPFLAGS+= -I$(CURLDIR)/include
SHLIB_LINK+= -L$(CURLDIR)/lib/.libs -lcurl -lssl -lcrypto

# for system version of curl, do this
#CURL_CONFIG:= $(shell which curl-config)
#PG_CPPFLAGS+= $(shell sh $(CURL_CONFIG) --cflags)
#SHLIB_LINK+= $(shell sh $(CUR_CONFIG) --static-libs)

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
