# BlueALSA - Makefile.am
# Copyright (c) 2016-2019 Arkadiusz Bokowy

bin_PROGRAMS =

if ENABLE_APLAY
bin_PROGRAMS += bluealsa-aplay
bluealsa_aplay_SOURCES = \
	../src/shared/dbus-client.c \
	../src/shared/ffb.c \
	../src/shared/log.c \
	aplay.c
bluealsa_aplay_CFLAGS = \
	-I$(top_srcdir)/src \
	@ALSA_CFLAGS@ \
	@BLUEZ_CFLAGS@ \
	@DBUS1_CFLAGS@
bluealsa_aplay_LDADD = \
	@ALSA_LIBS@ \
	@BLUEZ_LIBS@ \
	@DBUS1_LIBS@
endif

if ENABLE_RFCOMM
bin_PROGRAMS += bluealsa-rfcomm
bluealsa_rfcomm_SOURCES = \
	../src/shared/dbus-client.c \
	../src/shared/log.c \
	rfcomm.c
bluealsa_rfcomm_CFLAGS = \
	-I$(top_srcdir)/src \
	@BLUEZ_CFLAGS@ \
	@DBUS1_CFLAGS@
bluealsa_rfcomm_LDADD = \
	@DBUS1_LIBS@ \
	-lreadline
endif

if ENABLE_HCITOP
bin_PROGRAMS += hcitop
hcitop_CFLAGS = \
	@BLUEZ_CFLAGS@ \
	@LIBBSD_CFLAGS@ \
	@NCURSES_CFLAGS@
hcitop_LDADD = \
	@BLUEZ_LIBS@ \
	@LIBBSD_LIBS@ \
	@NCURSES_LIBS@
endif
