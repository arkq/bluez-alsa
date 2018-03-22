# BlueALSA - Makefile.am
# Copyright (c) 2016-2017 Arkadiusz Bokowy

include $(top_srcdir)/Makefile.am.include

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
	shared/bluealsa/ctl-client.h \
	shared/bluealsa/ctl-proto.h \
	shared/bluealsa/ffb.h \
	shared/bluealsa/log.h \
	shared/bluealsa/rt.h

lib_sources = \
	shared/bluealsa/ctl-client.c \
	shared/bluealsa/ffb.c \
	shared/bluealsa/log.c \
	shared/bluealsa/rt.c

lib_LTLIBRARIES = libbluealsa.la
libbluealsa_la_SOURCES = $(lib_headers) $(lib_sources)
libbluealsa_la_LDFLAGS = -version-info 0:0:0

AM_CFLAGS = \
	@BLUEZ_CFLAGS@ \
	@GLIB2_CFLAGS@ \
	@GIO2_CFLAGS@ \
	@AAC_CFLAGS@ \
	@APTX_CFLAGS@ \
	@SBC_CFLAGS@ \
	$(LOCAL_BLUEALSA_INCLUDES)

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
