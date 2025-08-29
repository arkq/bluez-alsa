# BlueALSA - bluealsactl/Makefile.am
# Copyright (c) 2016-2025 Arkadiusz Bokowy

if ENABLE_CTL

bin_PROGRAMS = bluealsactl

bluealsactl_SOURCES = \
	cmd-client-delay.c \
	cmd-codec.c \
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

AM_CFLAGS = \
	-I$(top_srcdir)/src \
	@DBUS1_CFLAGS@

bluealsactl_LDADD = \
	../shared/libshared.la \
	@DBUS1_LIBS@

endif
