# BlueALSA - Makefile.am
# Copyright (c) 2016-2025 Arkadiusz Bokowy

SUBDIRS = aplay rfcomm

bin_PROGRAMS =

if ENABLE_A2DPCONF
bin_PROGRAMS += a2dpconf
endif

if ENABLE_HCITOP
bin_PROGRAMS += hcitop
endif

AM_CFLAGS = \
	-I$(top_srcdir)/src \
	@BLUEZ_CFLAGS@ \
	@NCURSES_CFLAGS@

LDADD = \
	../src/shared/libshared.la \
	@BLUEZ_LIBS@ \
	@NCURSES_LIBS@
