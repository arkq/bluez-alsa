# BlueALSA - Makefile.am
# Copyright (c) 2016-2017 Arkadiusz Bokowy

bin_PROGRAMS =

if ENABLE_APLAY
bin_PROGRAMS += bluealsa-aplay
bluealsa_aplay_SOURCES = \
	../src/shared/ctl-client.c \
	../src/shared/log.c \
	aplay.c
bluealsa_aplay_CFLAGS = \
	-I$(top_srcdir)/src \
	@ALSA_CFLAGS@ \
	@BLUEZ_CFLAGS@
bluealsa_aplay_LDADD = \
	@ALSA_LIBS@ \
	@BLUEZ_LIBS@
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
