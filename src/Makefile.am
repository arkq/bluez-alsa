# BlueALSA - Makefile.am
# Copyright (c) 2016-2021 Arkadiusz Bokowy

bin_PROGRAMS = bluealsa
SUBDIRS = asound

dbusconfdir = @DBUS_CONF_DIR@
dist_dbusconf_DATA = bluealsa.conf

bluealsa_SOURCES = \
	shared/ffb.c \
	shared/log.c \
	shared/rt.c \
	a2dp.c \
	a2dp-sbc.c \
	at.c \
	audio.c \
	ba-adapter.c \
	ba-device.c \
	ba-rfcomm.c \
	ba-transport.c \
	bluealsa.c \
	bluealsa-dbus.c \
	bluealsa-iface.c \
	bluez.c \
	bluez-iface.c \
	bluez-skeleton.c \
	codec-sbc.c \
	dbus.c \
	hci.c \
	io.c \
	rtp.c \
	sco.c \
	utils.c \
	main.c

if ENABLE_AAC
bluealsa_SOURCES += \
	a2dp-aac.c
endif

if ENABLE_APTX
bluealsa_SOURCES += \
	a2dp-aptx.c
endif

if ENABLE_APTX_HD
bluealsa_SOURCES += \
	a2dp-aptx-hd.c
endif

if ENABLE_APTX_OR_APTX_HD
bluealsa_SOURCES += \
	codec-aptx.c
endif

if ENABLE_FASTSTREAM
bluealsa_SOURCES += \
	a2dp-faststream.c
endif

if ENABLE_LDAC
bluealsa_SOURCES += \
	a2dp-ldac.c
endif

if ENABLE_MPEG
bluealsa_SOURCES += \
	a2dp-mpeg.c
endif

if ENABLE_MSBC
bluealsa_SOURCES += \
	codec-msbc.c
endif

if ENABLE_OFONO
bluealsa_SOURCES += \
	ofono.c \
	ofono-iface.c
endif

if ENABLE_UPOWER
bluealsa_SOURCES += \
	upower.c
endif

AM_CFLAGS = \
	@AAC_CFLAGS@ \
	@APTX_CFLAGS@ \
	@APTX_HD_CFLAGS@ \
	@BLUEZ_CFLAGS@ \
	@GIO2_CFLAGS@ \
	@GLIB2_CFLAGS@ \
	@LDAC_ABR_CFLAGS@ \
	@LDAC_DEC_CFLAGS@ \
	@LDAC_ENC_CFLAGS@ \
	@LIBBSD_CFLAGS@ \
	@LIBUNWIND_CFLAGS@ \
	@MPG123_CFLAGS@ \
	@SBC_CFLAGS@

LDADD = \
	@AAC_LIBS@ \
	@APTX_HD_LIBS@ \
	@APTX_LIBS@ \
	@BLUEZ_LIBS@ \
	@GIO2_LIBS@ \
	@GLIB2_LIBS@ \
	@LDAC_ABR_LIBS@ \
	@LDAC_DEC_LIBS@ \
	@LDAC_ENC_LIBS@ \
	@LIBUNWIND_LIBS@ \
	@MP3LAME_LIBS@ \
	@MPG123_LIBS@ \
	@SBC_LIBS@
