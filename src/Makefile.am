# bluez-alsa - Makefile.am
# Copyright (c) 2016 Arkadiusz Bokowy

bin_PROGRAMS = bluealsa

asound_module_ctl_bluealsa_LTLIBRARIES = \
	libasound_module_ctl_bluealsa.la
asound_module_pcm_bluealsa_LTLIBRARIES = \
	libasound_module_pcm_bluealsa.la

asound_module_ctl_bluealsadir = @ALSA_PLUGIN_DIR@
asound_module_pcm_bluealsadir = @ALSA_PLUGIN_DIR@

bluealsa_SOURCES = \
	bluez.c \
	ctl.c \
	io.c \
	log.c \
	transport.c \
	utils.c \
	main.c

libasound_module_ctl_bluealsa_la_SOURCES = \
	log.c \
	alsa-ctl.c
libasound_module_pcm_bluealsa_la_SOURCES = \
	log.c \
	alsa-pcm.c

bluealsa_CFLAGS = \
	@ALSA_CFLAGS@ \
	@BLUEZ_CFLAGS@ \
	@DBUS1_CFLAGS@ \
	@GLIB2_CFLAGS@ \
	@SBC_CFLAGS@

bluealsa_LDADD = \
	@BLUEZ_LIBS@ \
	@DBUS1_LIBS@ \
	@GLIB2_LIBS@ \
	@SBC_LIBS@

AM_LDFLAGS = -module -avoid-version

libasound_module_ctl_bluealsa_la_LIBADD =
libasound_module_pcm_bluealsa_la_LIBADD = \
	@BLUEZ_LIBS@
