# BlueALSA - Makefile.am
# Copyright (c) 2016-2023 Arkadiusz Bokowy

asound_module_ctl_LTLIBRARIES = libasound_module_ctl_bluealsa.la
asound_module_pcm_LTLIBRARIES = libasound_module_pcm_bluealsa.la
asound_module_conf_DATA = 20-bluealsa.conf

libasound_module_ctl_bluealsa_la_SOURCES = \
	../shared/a2dp-codecs.c \
	../shared/dbus-client.c \
	../shared/dbus-client-pcm.c \
	bluealsa-ctl.c
libasound_module_pcm_bluealsa_la_SOURCES = \
	../shared/a2dp-codecs.c \
	../shared/dbus-client.c \
	../shared/dbus-client-pcm.c \
	../shared/hex.c \
	../shared/log.c \
	../shared/rt.c \
	bluealsa-pcm.c

asound_module_ctldir = @ALSA_PLUGIN_DIR@
asound_module_pcmdir = @ALSA_PLUGIN_DIR@
asound_module_confdir = @ALSA_CONF_DIR@

AM_CFLAGS = \
	-I$(top_srcdir)/src \
	@ALSA_CFLAGS@ \
	@BLUEZ_CFLAGS@ \
	@DBUS1_CFLAGS@ \
	@LIBBSD_CFLAGS@ \
	@LIBUNWIND_CFLAGS@

AM_LDFLAGS = -module -avoid-version

libasound_module_ctl_bluealsa_la_LIBADD = \
	@ALSA_LIBS@ \
	@DBUS1_LIBS@
libasound_module_pcm_bluealsa_la_LIBADD = \
	@ALSA_LIBS@ \
	@DBUS1_LIBS@ \
	@LIBUNWIND_LIBS@

SUFFIXES = .conf.in .conf
MOSTLYCLEANFILES = $(asound_module_conf_DATA)

.conf.in.conf:
	$(SED) -e '' < $< > $@
