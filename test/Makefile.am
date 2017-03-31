# BlueALSA - Makefile.am
# Copyright (c) 2016-2017 Arkadiusz Bokowy

TESTS = \
	test-io \
	test-utils

check_PROGRAMS = \
	test-io \
	test-pcm \
	test-server \
	test-utils

AM_CFLAGS = \
	-I$(top_srcdir)/src \
	@ALSA_CFLAGS@ \
	@BLUEZ_CFLAGS@ \
	@GLIB2_CFLAGS@ \
	@GIO2_CFLAGS@ \
	@AAC_CFLAGS@ \
	@SBC_CFLAGS@

LDADD = \
	@ALSA_LIBS@ \
	@BLUEZ_LIBS@ \
	@GLIB2_LIBS@ \
	@GIO2_LIBS@ \
	@AAC_LIBS@ \
	@SBC_LIBS@
