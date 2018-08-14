# BlueALSA - Makefile.am
# Copyright (c) 2016-2018 Arkadiusz Bokowy

TESTS = \
	test-at \
	test-io \
	test-utils

if ENABLE_PCM_TEST
TESTS += test-pcm
endif

check_PROGRAMS = \
	server-mock \
	test-at \
	test-io \
	test-pcm \
	test-utils

AM_CFLAGS = \
	-I$(top_srcdir)/src \
	@ALSA_CFLAGS@ \
	@BLUEZ_CFLAGS@ \
	@GLIB2_CFLAGS@ \
	@GIO2_CFLAGS@ \
	@AAC_CFLAGS@ \
	@APTX_CFLAGS@ \
	@SBC_CFLAGS@

LDADD = \
	@ALSA_LIBS@ \
	@BLUEZ_LIBS@ \
	@GLIB2_LIBS@ \
	@GIO2_LIBS@ \
	@AAC_LIBS@ \
	@APTX_LIBS@ \
	@SBC_LIBS@
