# BlueALSA - Makefile.am
# Copyright (c) 2016-2023 Arkadiusz Bokowy

check_PROGRAMS = \
	bluealsa-mock

bluealsa_mock_SOURCES = \
	../../src/shared/a2dp-codecs.c \
	../../src/shared/ffb.c \
	../../src/shared/log.c \
	../../src/shared/rt.c \
	../../src/a2dp.c \
	../../src/a2dp-sbc.c \
	../../src/at.c \
	../../src/audio.c \
	../../src/ba-adapter.c \
	../../src/ba-device.c \
	../../src/ba-rfcomm.c \
	../../src/ba-transport.c \
	../../src/ba-transport-pcm.c \
	../../src/bluealsa-config.c \
	../../src/bluealsa-dbus.c \
	../../src/bluealsa-iface.c \
	../../src/codec-sbc.c \
	../../src/dbus.c \
	../../src/hci.c \
	../../src/hfp.c \
	../../src/io.c \
	../../src/rtp.c \
	../../src/sco.c \
	../../src/storage.c \
	../../src/utils.c \
	mock-bluealsa.c \
	mock-bluez.c \
	mock.c

bluealsa_mock_CFLAGS = \
	-I$(top_srcdir)/src \
	-I$(top_srcdir)/test \
	@ALSA_CFLAGS@ \
	@BLUEZ_CFLAGS@ \
	@GIO2_CFLAGS@ \
	@GLIB2_CFLAGS@ \
	@LIBBSD_CFLAGS@ \
	@LIBUNWIND_CFLAGS@ \
	@SBC_CFLAGS@ \
	@SPANDSP_CFLAGS@

bluealsa_mock_LDADD = \
	@ALSA_LIBS@ \
	@BLUEZ_LIBS@ \
	@GIO2_LIBS@ \
	@GLIB2_LIBS@ \
	@LIBUNWIND_LIBS@ \
	@SBC_LIBS@ \
	@SPANDSP_LIBS@

if ENABLE_AAC
bluealsa_mock_SOURCES += ../../src/a2dp-aac.c
bluealsa_mock_CFLAGS += @AAC_CFLAGS@
bluealsa_mock_LDADD += @AAC_LIBS@
endif

if ENABLE_APTX
bluealsa_mock_SOURCES += ../../src/a2dp-aptx.c
bluealsa_mock_CFLAGS += @APTX_CFLAGS@
bluealsa_mock_LDADD += @APTX_LIBS@
endif

if ENABLE_APTX_HD
bluealsa_mock_SOURCES += ../../src/a2dp-aptx-hd.c
bluealsa_mock_CFLAGS += @APTX_HD_CFLAGS@
bluealsa_mock_LDADD += @APTX_HD_LIBS@
endif

if ENABLE_APTX_OR_APTX_HD
bluealsa_mock_SOURCES += ../../src/codec-aptx.c
endif

if ENABLE_FASTSTREAM
bluealsa_mock_SOURCES += ../../src/a2dp-faststream.c
endif

if ENABLE_LC3PLUS
bluealsa_mock_SOURCES += ../../src/a2dp-lc3plus.c
bluealsa_mock_LDADD += @LC3PLUS_LIBS@
endif

if ENABLE_LDAC
bluealsa_mock_SOURCES += ../../src/a2dp-ldac.c
bluealsa_mock_CFLAGS += @LDAC_ABR_CFLAGS@ @LDAC_DEC_CFLAGS@ @LDAC_ENC_CFLAGS@
bluealsa_mock_LDADD += @LDAC_ABR_LIBS@ @LDAC_DEC_LIBS@ @LDAC_ENC_LIBS@
endif

if ENABLE_MIDI
bluealsa_mock_SOURCES += \
	../../src/ble-midi.c \
	../../src/midi.c
endif

if ENABLE_MPEG
bluealsa_mock_SOURCES += ../../src/a2dp-mpeg.c
bluealsa_mock_CFLAGS += @MPG123_CFLAGS@
bluealsa_mock_LDADD += @MP3LAME_LIBS@ @MPG123_LIBS@
endif

if ENABLE_MSBC
bluealsa_mock_SOURCES += ../../src/codec-msbc.c
endif
