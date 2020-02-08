# BlueALSA - Makefile.am
# Copyright (c) 2016-2020 Arkadiusz Bokowy

TESTS = \
	test-alsa-ctl \
	test-alsa-pcm \
	test-at \
	test-ba \
	test-io \
	test-msbc \
	test-rfcomm \
	test-utils

check_PROGRAMS = \
	server-mock \
	test-alsa-ctl \
	test-alsa-pcm \
	test-at \
	test-ba \
	test-io \
	test-msbc \
	test-rfcomm \
	test-utils

check_LTLIBRARIES = \
	aloader.la
aloader_la_LDFLAGS = \
	-rpath /nowhere \
	-avoid-version \
	-shared -module

AM_CFLAGS = \
	-I$(top_srcdir)/src \
	@AAC_CFLAGS@ \
	@ALSA_CFLAGS@ \
	@APTX_CFLAGS@ \
	@APTX_HD_CFLAGS@ \
	@BLUEZ_CFLAGS@ \
	@CHECK_CFLAGS@ \
	@GIO2_CFLAGS@ \
	@GLIB2_CFLAGS@ \
	@LDAC_ABR_CFLAGS@ \
	@LDAC_CFLAGS@ \
	@MPG123_CFLAGS@ \
	@SBC_CFLAGS@

LDADD = \
	@AAC_LIBS@ \
	@ALSA_LIBS@ \
	@APTX_LIBS@ \
	@APTX_HD_LIBS@ \
	@BLUEZ_LIBS@ \
	@CHECK_LIBS@ \
	@GIO2_LIBS@ \
	@GLIB2_LIBS@ \
	@LDAC_ABR_LIBS@ \
	@LDAC_LIBS@ \
	@MP3LAME_LIBS@ \
	@MPG123_LIBS@ \
	@SBC_LIBS@
