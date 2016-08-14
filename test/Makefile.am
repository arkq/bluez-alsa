# bluez-alsa - Makefile.am
# Copyright (c) 2016 Arkadiusz Bokowy

TESTS = \
	test-io

check_PROGRAMS = \
	test-io

AM_CFLAGS = \
	-DSRCDIR=\"$(srcdir)\" \
	@BLUEZ_CFLAGS@ \
	@GLIB2_CFLAGS@ \
	@GIO20_CFLAGS@ \
	@SBC_CFLAGS@

LDADD = \
	@BLUEZ_LIBS@ \
	@GLIB2_LIBS@ \
	@GIO20_LIBS@ \
	@SBC_LIBS@
