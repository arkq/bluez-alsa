# BlueALSA - Makefile.am
# Copyright (c) 2016 Arkadiusz Bokowy

TESTS = \
	test-io \
	test-utils

check_PROGRAMS = \
	test-io \
	test-pcm \
	test-utils

AM_CFLAGS = \
	-I$(top_srcdir)/src \
	-DSRCDIR=\"$(srcdir)\" \
	@BLUEZ_CFLAGS@ \
	@GLIB2_CFLAGS@ \
	@GIO2_CFLAGS@ \
	@AAC_CFLAGS@ \
	@SBC_CFLAGS@

LDADD = \
	@BLUEZ_LIBS@ \
	@GLIB2_LIBS@ \
	@GIO2_LIBS@ \
	@AAC_LIBS@ \
	@SBC_LIBS@
