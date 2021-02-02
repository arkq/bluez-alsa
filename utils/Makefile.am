# BlueALSA - Makefile.am
# Copyright (c) 2016-2020 Arkadiusz Bokowy

SUBDIRS = aplay cmd rfcomm

bin_PROGRAMS =

if ENABLE_A2DPCONF
bin_PROGRAMS += a2dpconf
a2dpconf_CFLAGS = \
	-I$(top_srcdir)/src
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
