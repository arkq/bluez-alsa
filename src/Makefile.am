# bluez-alsa - Makefile.am
# Copyright (c) 2016 Arkadiusz Bokowy

bin_PROGRAMS = bluealsa
SUBDIRS = alsa

bluealsa_SOURCES = \
	bluez.c \
	ctl.c \
	device.c \
	io.c \
	log.c \
	transport.c \
	utils.c \
	main.c

AM_CFLAGS = \
	@BLUEZ_CFLAGS@ \
	@DBUS1_CFLAGS@ \
	@GLIB2_CFLAGS@ \
	@SBC_CFLAGS@

LDADD = \
	@BLUEZ_LIBS@ \
	@DBUS1_LIBS@ \
	@GLIB2_LIBS@ \
	@SBC_LIBS@
