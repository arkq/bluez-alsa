# BlueALSA - Makefile.am
# Copyright (c) 2016-2025 Arkadiusz Bokowy

if ENABLE_APLAY

bin_PROGRAMS = bluealsa-aplay

bluealsa_aplay_SOURCES = \
	alsa-mixer.c \
	alsa-pcm.c \
	dbus.c \
	delay-report.c \
	aplay.c

if WITH_LIBSAMPLERATE
bluealsa_aplay_SOURCES += \
	resampler.c
endif

AM_CFLAGS = \
	-I$(top_srcdir)/src \
	@ALSA_CFLAGS@ \
	@BLUEZ_CFLAGS@ \
	@DBUS1_CFLAGS@ \
	@SAMPLERATE_CFLAGS@

bluealsa_aplay_LDADD = \
	../../src/shared/libshared.la \
	@ALSA_LIBS@ \
	@BLUEZ_LIBS@ \
	@DBUS1_LIBS@ \
	@SAMPLERATE_LIBS@

endif
