# BlueALSA - Makefile.am
# Copyright (c) 2016-2022 Arkadiusz Bokowy

if ENABLE_CLI

bin_PROGRAMS = bluealsa-cli

bluealsa_cli_SOURCES = \
	../../src/shared/a2dp-codecs.c \
	../../src/shared/dbus-client.c \
	../../src/shared/dbus-client-pcm.c \
	../../src/shared/hex.c \
	../../src/shared/log.c \
	cmd-codec.c \
	cmd-delay-adjustment.c \
	cmd-info.c \
	cmd-list-pcms.c \
	cmd-list-services.c \
	cmd-monitor.c \
	cmd-mute.c \
	cmd-open.c \
	cmd-softvol.c \
	cmd-status.c \
	cmd-volume.c \
	cli.c

bluealsa_cli_CFLAGS = \
	-I$(top_srcdir)/src \
	@DBUS1_CFLAGS@ \
	@LIBUNWIND_CFLAGS@

bluealsa_cli_LDADD = \
	@DBUS1_LIBS@ \
	@LIBUNWIND_LIBS@

endif
