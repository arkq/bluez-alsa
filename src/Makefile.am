# BlueALSA - Makefile.am
# Copyright (c) 2016-2017 Arkadiusz Bokowy

bin_PROGRAMS = bluealsa
SUBDIRS = asound

bluealsa_SOURCES = \
	shared/ffb.c \
	shared/log.c \
	shared/rt.c \
	at.c \
	bluealsa.c \
	bluez.c \
	bluez-a2dp.c \
	bluez-iface.c \
	ctl.c \
	io.c \
	rfcomm.c \
	transport.c \
	utils.c \
	main.c

AM_CFLAGS = \
	@BLUEZ_CFLAGS@ \
	@GLIB2_CFLAGS@ \
	@GIO2_CFLAGS@ \
	@AAC_CFLAGS@ \
	@APTX_CFLAGS@ \
	@SBC_CFLAGS@

LDADD = \
	@BLUEZ_LIBS@ \
	@GLIB2_LIBS@ \
	@GIO2_LIBS@ \
	@AAC_LIBS@ \
	@APTX_LIBS@ \
	@SBC_LIBS@
