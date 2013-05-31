# contrib/json_fdw/Makefile

MODULE_big = json_fdw

OBJS = json_fdw.o
SHLIB_LINK = -lz -l:libyajl.so.2

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

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
