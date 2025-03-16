# BlueALSA - Makefile.am
# Copyright (c) 2016-2022 Arkadiusz Bokowy

ACLOCAL_AMFLAGS = -I m4
SUBDIRS = misc src utils

if ENABLE_MANPAGES
SUBDIRS += doc
endif

if ENABLE_TEST
SUBDIRS += test
endif

if WITH_COVERAGE
cov:
	$(MAKE) $(AM_MAKEFLAGS) check CFLAGS="$(CFLAGS) -O0 --coverage -fprofile-update=atomic"
	$(LCOV) --capture -d src -d utils -d test \
		--exclude '$(abs_builddir)/*' --exclude '/usr/*' --exclude '*/test/*' \
		--output-file lcov.info
	$(GENHTML) -o coverage -t $(PACKAGE) lcov.info
clean-local:
	find $(top_builddir) -name "*.gcno" -delete
	find $(top_builddir) -name "*.gcda" -delete
	rm -rf coverage lcov.info
endif
