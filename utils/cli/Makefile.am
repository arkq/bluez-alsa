# BlueALSA - Makefile.am
# Copyright (c) 2016-2021 Arkadiusz Bokowy

if ENABLE_CLI

bin_PROGRAMS = bluealsa-cli

bluealsa_cli_SOURCES = \
	../../src/shared/dbus-client.c \
	../../src/shared/log.c \
	cli.c

bluealsa_cli_CFLAGS = \
	-I$(top_srcdir)/src \
	@DBUS1_CFLAGS@ \
	@LIBUNWIND_CFLAGS@

bluealsa_cli_LDADD = \
	@DBUS1_LIBS@ \
	@LIBUNWIND_LIBS@

endif
