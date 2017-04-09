# BlueALSA - Makefile.am
# Copyright (c) 2016-2017 Arkadiusz Bokowy

EXTRA_DIST = 20-bluealsa.conf

asound_module_ctl_LTLIBRARIES = libasound_module_ctl_bluealsa.la
asound_module_pcm_LTLIBRARIES = libasound_module_pcm_bluealsa.la
asound_module_data_DATA = 20-bluealsa.conf

libasound_module_ctl_bluealsa_la_SOURCES = \
	../shared/ctl-client.c \
	../shared/log.c \
	bluealsa-ctl.c
libasound_module_pcm_bluealsa_la_SOURCES = \
	../shared/ctl-client.c \
	../shared/log.c \
	../shared/rt.c \
	bluealsa-pcm.c

asound_module_ctldir = @ALSA_PLUGIN_DIR@
asound_module_pcmdir = @ALSA_PLUGIN_DIR@
asound_module_datadir = @ALSA_DATA_DIR@/alsa.conf.d

AM_CFLAGS = \
	-I$(top_srcdir)/src \
	@ALSA_CFLAGS@ \
	@BLUEZ_CFLAGS@ \
	@GLIB2_CFLAGS@

AM_LDFLAGS = -module -avoid-version

libasound_module_ctl_bluealsa_la_LIBADD = \
	@ALSA_LIBS@
libasound_module_pcm_bluealsa_la_LIBADD = \
	@ALSA_LIBS@ \
	@BLUEZ_LIBS@
