# BlueALSA - Makefile.am
# Copyright (c) 2016-2019 Arkadiusz Bokowy

bin_PROGRAMS = bluealsa
SUBDIRS = asound

dbusconfdir = @DBUS_CONF_DIR@
dist_dbusconf_DATA = bluealsa.conf

bluealsa_SOURCES = \
	shared/ffb.c \
	shared/log.c \
	shared/rt.c \
	at.c \
	ba-adapter.c \
	ba-device.c \
	ba-transport.c \
	bluealsa.c \
	bluealsa-dbus.c \
	bluealsa-iface.c \
	bluez.c \
	bluez-a2dp.c \
	bluez-iface.c \
	io.c \
	rfcomm.c \
	utils.c \
	main.c

if ENABLE_MSBC
bluealsa_SOURCES += \
	msbc.c
endif

if ENABLE_OFONO
bluealsa_SOURCES += \
	ofono.c \
	ofono-iface.c
endif

AM_CFLAGS = \
	@BLUEZ_CFLAGS@ \
	@GLIB2_CFLAGS@ \
	@GIO2_CFLAGS@ \
	@AAC_CFLAGS@ \
	@APTX_CFLAGS@ \
	@LDAC_CFLAGS@ \
	@LDAC_ABR_CFLAGS@ \
	@SBC_CFLAGS@

LDADD = \
	@BLUEZ_LIBS@ \
	@GLIB2_LIBS@ \
	@GIO2_LIBS@ \
	@AAC_LIBS@ \
	@APTX_LIBS@ \
	@LDAC_LIBS@ \
	@LDAC_ABR_LIBS@ \
	@SBC_LIBS@
