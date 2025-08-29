# BlueALSA - Makefile.am
# Copyright (c) 2025 Arkadiusz Bokowy

noinst_LTLIBRARIES = libshared.la

libshared_la_SOURCES = \
	a2dp-codecs.c \
	dbus-client-pcm.c \
	dbus-client-rfcomm.c \
	dbus-client.c \
	ffb.c \
	hex.c \
	log.c \
	nv.c \
	rt.c

AM_CFLAGS = \
	@BLUEZ_CFLAGS@ \
	@DBUS1_CFLAGS@ \
	@LIBBSD_CFLAGS@ \
	@LIBUNWIND_CFLAGS@

libshared_la_LIBADD = \
	@BLUEZ_LIBS@ \
	@DBUS1_LIBS@ \
	@LIBBSD_LIBS@ \
	@LIBUNWIND_LIBS@
