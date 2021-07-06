# BlueALSA - Makefile.am
# Copyright (c) 2016-2021 Arkadiusz Bokowy

TESTS = \
	test-a2dp \
	test-alsa-ctl \
	test-alsa-pcm \
	test-at \
	test-audio \
	test-ba \
	test-io \
	test-rfcomm \
	test-utils

check_PROGRAMS = \
	bluealsa-mock \
	test-a2dp \
	test-alsa-ctl \
	test-alsa-pcm \
	test-at \
	test-audio \
	test-ba \
	test-io \
	test-rfcomm \
	test-utils

if ENABLE_MSBC
TESTS += test-msbc
check_PROGRAMS += test-msbc
endif

check_LTLIBRARIES = \
	aloader.la
aloader_la_LDFLAGS = \
	-rpath /nowhere \
	-avoid-version \
	-shared -module

bluealsa_mock_SOURCES = \
	../src/shared/ffb.c \
	../src/shared/log.c \
	../src/shared/rt.c \
	../src/a2dp-audio.c \
	../src/a2dp-rtp.c \
	../src/at.c \
	../src/audio.c \
	../src/ba-adapter.c \
	../src/ba-device.c \
	../src/ba-rfcomm.c \
	../src/ba-transport.c \
	../src/bluealsa-dbus.c \
	../src/bluealsa-iface.c \
	../src/bluealsa.c \
	../src/codec-sbc.c \
	../src/dbus.c \
	../src/hci.c \
	../src/io.c \
	../src/sco.c \
	../src/utils.c \
	bluealsa-mock.c

test_a2dp_SOURCES = \
	../src/shared/log.c \
	../src/bluealsa.c \
	test-a2dp.c

test_alsa_ctl_SOURCES = \
	../src/shared/log.c \
	test-alsa-ctl.c

test_alsa_pcm_SOURCES = \
	../src/shared/log.c \
	../src/shared/rt.c \
	test-alsa-pcm.c

test_at_SOURCES = \
	../src/shared/log.c \
	../src/at.c \
	test-at.c

test_audio_SOURCES = \
	../src/shared/log.c \
	../src/audio.c \
	test-audio.c

test_ba_SOURCES = \
	../src/shared/log.c \
	../src/audio.c \
	../src/ba-adapter.c \
	../src/ba-device.c \
	../src/bluealsa.c \
	../src/dbus.c \
	../src/hci.c \
	../src/utils.c \
	test-ba.c

test_io_SOURCES = \
	../src/shared/ffb.c \
	../src/shared/log.c \
	../src/shared/rt.c \
	../src/a2dp-rtp.c \
	../src/audio.c \
	../src/ba-adapter.c \
	../src/ba-device.c \
	../src/bluealsa.c \
	../src/codec-sbc.c \
	../src/dbus.c \
	../src/hci.c \
	../src/io.c \
	../src/sco.c \
	../src/utils.c \
	test-io.c

if ENABLE_MSBC
test_msbc_SOURCES = \
	../src/shared/ffb.c \
	../src/shared/log.c \
	../src/codec-sbc.c \
	test-msbc.c
endif

test_rfcomm_SOURCES = \
	../src/shared/log.c \
	../src/a2dp.c \
	../src/at.c \
	../src/audio.c \
	../src/ba-adapter.c \
	../src/ba-device.c \
	../src/ba-rfcomm.c \
	../src/ba-transport.c \
	../src/bluealsa.c \
	../src/dbus.c \
	../src/hci.c \
	../src/utils.c \
	test-rfcomm.c

test_utils_SOURCES = \
	../src/shared/ffb.c \
	../src/shared/log.c \
	../src/shared/rt.c \
	../src/hci.c \
	../src/utils.c \
	test-utils.c

if ENABLE_APTX_OR_APTX_HD
bluealsa_mock_SOURCES += ../src/codec-aptx.c
test_io_SOURCES += ../src/codec-aptx.c
endif

if ENABLE_MSBC
bluealsa_mock_SOURCES += ../src/codec-msbc.c
test_io_SOURCES += ../src/codec-msbc.c
endif

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
	@LDAC_DEC_CFLAGS@ \
	@LDAC_ENC_CFLAGS@ \
	@LIBUNWIND_CFLAGS@ \
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
	@LDAC_DEC_LIBS@ \
	@LDAC_ENC_LIBS@ \
	@LIBUNWIND_LIBS@ \
	@MP3LAME_LIBS@ \
	@MPG123_LIBS@ \
	@SBC_LIBS@
