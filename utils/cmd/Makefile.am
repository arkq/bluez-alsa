# BlueALSA - Makefile.am
# Copyright (c) 2016-2021 Arkadiusz Bokowy

if ENABLE_CMD

bin_PROGRAMS = bluealsa-cmd

bluealsa_cmd_SOURCES = \
	../../src/shared/dbus-client.c \
	../../src/shared/log.c \
	../dbus.c \
	cmd.c

bluealsa_cmd_CFLAGS = \
	-I$(top_srcdir)/src \
	@BLUEZ_CFLAGS@ \
	@DBUS1_CFLAGS@ \
	@LIBUNWIND_CFLAGS@

bluealsa_cmd_LDADD = \
	@BLUEZ_LIBS@ \
	@DBUS1_LIBS@ \
	@LIBUNWIND_LIBS@

endif
