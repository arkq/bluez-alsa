# BlueALSA - Makefile.am
# Copyright (c) 2016-2025 Arkadiusz Bokowy

SUBDIRS = mock

TESTS = \
	test-a2dp \
	test-alsa-ctl \
	test-alsa-pcm \
	test-alsa-pcm-hwcompat-busy \
	test-alsa-pcm-hwcompat-none \
	test-alsa-pcm-hwcompat-silence \
	test-at \
	test-audio \
	test-ba \
	test-dbus \
	test-h2 \
	test-io \
	test-rfcomm \
	test-rtp \
	test-utils

check_PROGRAMS = \
	test-a2dp \
	test-alsa-ctl \
	test-alsa-pcm \
	test-at \
	test-audio \
	test-ba \
	test-dbus \
	test-h2 \
	test-io \
	test-rfcomm \
	test-rtp \
	test-utils

if ENABLE_APLAY
TESTS += test-utils-aplay
check_PROGRAMS += test-utils-aplay
endif

if ENABLE_CTL
TESTS += test-utils-ctl
check_PROGRAMS += test-utils-ctl
endif

if ENABLE_LC3_SWB
TESTS += test-lc3-swb
check_PROGRAMS += test-lc3-swb
endif

if ENABLE_MIDI
TESTS += test-alsa-midi test-ble-midi
check_PROGRAMS += test-alsa-midi test-ble-midi
endif

if ENABLE_MSBC
TESTS += test-msbc
check_PROGRAMS += test-msbc
endif

if HAVE_SNDFILE
check_PROGRAMS += sndalign
endif

check_LTLIBRARIES = \
	libaloader.la
libaloader_la_LDFLAGS = \
	-rpath /nowhere \
	-avoid-version \
	-shared
libaloader_la_LIBADD = \
	@ALSA_LIBS@

BUILT_SOURCES = \
	test-dbus-iface.c

MOSTLYCLEANFILES = \
	$(BUILT_SOURCES)

test_dbus_SOURCES = \
	test-dbus-iface.c \
	test-dbus.c

AM_TESTS_ENVIRONMENT = \
	export G_DEBUG=fatal-warnings;

AM_CFLAGS = \
	-I$(top_srcdir)/src \
	@ALSA_CFLAGS@ \
	@BLUEZ_CFLAGS@ \
	@CHECK_CFLAGS@ \
	@GIO2_CFLAGS@ \
	@GLIB2_CFLAGS@ \
	@LDAC_ABR_CFLAGS@ \
	@LDAC_DEC_CFLAGS@ \
	@LDAC_ENC_CFLAGS@ \
	@LHDC_DEC_CFLAGS@ \
	@LHDC_ENC_CFLAGS@ \
	@OPENMP_CFLAGS@ \
	@SNDFILE_CFLAGS@

LDADD = \
	../src/shared/libshared.la \
	../src/libbluealsad.la \
	@ALSA_LIBS@ \
	@BLUEZ_LIBS@ \
	@CHECK_LIBS@ \
	@GIO2_LIBS@ \
	@GLIB2_LIBS@ \
	@SNDFILE_LIBS@

.xml.c:
	$(top_srcdir)/src/dbus-codegen.py --output $@ \
		--interface-info-body --interface-skeleton-body $<
