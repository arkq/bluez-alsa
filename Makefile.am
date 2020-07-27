# BlueALSA - Makefile.am
# Copyright (c) 2016-2020 Arkadiusz Bokowy

ACLOCAL_AMFLAGS = -I m4
SUBDIRS = src utils

if ENABLE_TEST
SUBDIRS += test
endif

if WITH_COVERAGE
cov:
	$(MAKE) $(AM_MAKEFLAGS) check CFLAGS="$(CFLAGS) -O0 --coverage"
	$(LCOV) --capture -d src -d test --exclude '/usr/*' --exclude "*/test/*" -o cov.info
	$(GENHTML) -o coverage -t $(PACKAGE) cov.info
clean-local:
	find $(top_builddir) -name "*.gcno" -delete
	find $(top_builddir) -name "*.gcda" -delete
	rm -rf coverage cov.info
endif
