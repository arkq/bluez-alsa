# BlueALSA - bluealsactl/Makefile.am
# Copyright (c) 2016-2024 Arkadiusz Bokowy

if ENABLE_CTL

bin_PROGRAMS = bluealsactl

bluealsactl_SOURCES = \
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
	main.c

bluealsactl_CFLAGS = \
	-I$(top_srcdir)/src \
	@DBUS1_CFLAGS@ \
	@LIBUNWIND_CFLAGS@

bluealsactl_LDADD = \
	@DBUS1_LIBS@ \
	@LIBUNWIND_LIBS@

endif
