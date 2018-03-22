# BlueALSA - Makefile.am
# Copyright (c) 2016-2017 Arkadiusz Bokowy

bin_PROGRAMS = bluealsa
# put . so current directory is built before subdirectory for dependencies
# TODO: wouldn't be necessary with non-recursive make or meson
SUBDIRS = . asound

bluealsa_SOURCES = \
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

lib_headers = \
	shared/ctl-client.h \
	shared/ctl-proto.h \
	shared/ffb.h \
	shared/log.h \
	shared/rt.h

lib_sources = \
	shared/ctl-client.c \
	shared/ffb.c \
	shared/log.c \
	shared/rt.c

lib_LTLIBRARIES = libbluealsa.la
libbluealsa_la_SOURCES = $(lib_headers) $(lib_sources)
libbluealsa_la_LDFLAGS = -version-info 0:0:0


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
	@SBC_LIBS@ \
	libbluealsa.la

# Install headers
bluealsaincludedir = $(includedir)/bluealsa
bluealsainclude_HEADERS = $(lib_headers)
