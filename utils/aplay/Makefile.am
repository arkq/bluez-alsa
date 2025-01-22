# BlueALSA - Makefile.am
# Copyright (c) 2016-2025 Arkadiusz Bokowy

if ENABLE_APLAY

bin_PROGRAMS = bluealsa-aplay

bluealsa_aplay_SOURCES = \
	../../src/shared/a2dp-codecs.c \
	../../src/shared/dbus-client.c \
	../../src/shared/dbus-client-pcm.c \
	../../src/shared/ffb.c \
	../../src/shared/log.c \
	../../src/shared/nv.c \
	../../src/shared/rt.c \
	alsa-mixer.c \
	alsa-pcm.c \
	dbus.c \
	delay-report.c \
	aplay.c

if WITH_LIBSAMPLERATE
bluealsa_aplay_SOURCES += \
	resampler.c
endif

bluealsa_aplay_CFLAGS = \
	-I$(top_srcdir)/src \
	@ALSA_CFLAGS@ \
	@BLUEZ_CFLAGS@ \
	@DBUS1_CFLAGS@ \
	@LIBUNWIND_CFLAGS@ \
	@SAMPLERATE_CFLAGS@

bluealsa_aplay_LDADD = \
	@ALSA_LIBS@ \
	@BLUEZ_LIBS@ \
	@DBUS1_LIBS@ \
	@LIBUNWIND_LIBS@ \
	@SAMPLERATE_LIBS@

endif
