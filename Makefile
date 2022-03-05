# FILES = $(shell which pg_config)

# all:
# 	echo $(FILES)
PG_CONFIG := pg_config
PROGRAM    = pg2arrow

OBJS = pg2arrow.o query.o arrow_types.o arrow_read.o arrow_write.o arrow_dump.o

# These don't look applied to gcc
PG_CPPFLAGS = -I$(shell $(PG_CONFIG) --includedir)
PG_CPPFLAGS += -I/usr/lib/postgresql/12/lib

PG_LDFLAGS = -L/usr/lib/postgresql/12/lib
PG_LIBS = -lpq

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
