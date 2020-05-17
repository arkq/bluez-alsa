# BlueALSA - Makefile.am
# Copyright (c) 2016-2020 Arkadiusz Bokowy

if ENABLE_RFCOMM

bin_PROGRAMS = bluealsa-rfcomm

bluealsa_rfcomm_SOURCES = \
	../../src/shared/dbus-client.c \
	../../src/shared/log.c \
	rfcomm.c

bluealsa_rfcomm_CFLAGS = \
	-I$(top_srcdir)/src \
	@BLUEZ_CFLAGS@ \
	@DBUS1_CFLAGS@ \
	@LIBUNWIND_CFLAGS@

bluealsa_rfcomm_LDADD = \
	@DBUS1_LIBS@ \
	@LIBUNWIND_LIBS@ \
	-lreadline

endif
