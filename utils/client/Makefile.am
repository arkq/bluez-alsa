# BlueALSA - Makefile.am
# Copyright (c) 2016-2021 Arkadiusz Bokowy

if ENABLE_CLIENT

bin_PROGRAMS = bluealsa-client

bluealsa_client_SOURCES = \
	../../src/shared/dbus-client.c \
	../../src/shared/log.c \
	../dbus.c \
	client.c

bluealsa_client_CFLAGS = \
	-I$(top_srcdir)/src \
	@DBUS1_CFLAGS@ \
	@LIBUNWIND_CFLAGS@

bluealsa_client_LDADD = \
	@BLUEZ_LIBS@ \
	@DBUS1_LIBS@ \
	@LIBUNWIND_LIBS@

endif
