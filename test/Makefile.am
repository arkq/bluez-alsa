# BlueALSA - Makefile.am
# Copyright (c) 2016-2018 Arkadiusz Bokowy

TESTS = \
	test-at \
	test-io \
	test-utils

if ENABLE_TEST_PCM
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
	@AAC_CFLAGS@ \
	@ALSA_CFLAGS@ \
	@APTX_CFLAGS@ \
	@BLUEZ_CFLAGS@ \
	@CHECK_CFLAGS@ \
	@GIO2_CFLAGS@ \
	@GLIB2_CFLAGS@ \
	@LDAC_ABR_CFLAGS@ \
	@LDAC_CFLAGS@ \
	@SBC_CFLAGS@

LDADD = \
	@AAC_LIBS@ \
	@ALSA_LIBS@ \
	@APTX_LIBS@ \
	@BLUEZ_LIBS@ \
	@CHECK_LIBS@ \
	@GIO2_LIBS@ \
	@GLIB2_LIBS@ \
	@LDAC_ABR_LIBS@ \
	@LDAC_LIBS@ \
	@SBC_LIBS@
