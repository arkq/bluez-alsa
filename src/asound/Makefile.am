# BlueALSA - Makefile.am
# Copyright (c) 2016-2022 Arkadiusz Bokowy

EXTRA_DIST = 20-bluealsa.conf 21-bluealsa-hfp.conf

asound_module_ctl_LTLIBRARIES = libasound_module_ctl_bluealsa.la
asound_module_pcm_LTLIBRARIES = libasound_module_pcm_bluealsa.la
asound_module_hook_LTLIBRARIES = libasound_module_pcm_hooks_bluealsa.la
asound_module_conf_DATA = 20-bluealsa.conf  21-bluealsa-hfp.conf

libasound_module_ctl_bluealsa_la_SOURCES = \
	../shared/a2dp-codecs.c \
	../shared/dbus-client.c \
	bluealsa-ctl.c
libasound_module_pcm_bluealsa_la_SOURCES = \
	../shared/a2dp-codecs.c \
	../shared/dbus-client.c \
	../shared/hex.c \
	../shared/log.c \
	../shared/rt.c \
	bluealsa-pcm.c
libasound_module_pcm_hooks_bluealsa_la_SOURCES = \
	../shared/a2dp-codecs.c \
	../shared/dbus-client.c \
	hfp-session.c \
	hfp-hook.c
asound_module_ctldir = @ALSA_PLUGIN_DIR@
asound_module_pcmdir = @ALSA_PLUGIN_DIR@
asound_module_hookdir = @ALSA_PLUGIN_DIR@
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
libasound_module_pcm_hooks_bluealsa_la_LIBADD = \
	@ALSA_LIBS@ \
	@DBUS1_LIBS@
