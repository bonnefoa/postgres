# src/test/modules/test_notify_rollback/Makefile

MODULE_big = test_notify_rollback
OBJS = \
	$(WIN32RES) \
	test_notify_rollback.o
PGFILEDESC = "test_notify_rollback - example use of object access hooks"

REGRESS = test_notify_rollback

# disable installcheck for now
NO_INSTALLCHECK = 1
# and also for now force NO_LOCALE and UTF8
ENCODING = UTF8
NO_LOCALE = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_notify_rollback
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
