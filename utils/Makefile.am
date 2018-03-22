# BlueALSA - Makefile.am
# Copyright (c) 2016-2018 Arkadiusz Bokowy

include $(top_srcdir)/Makefile.am.include

bin_PROGRAMS =

if ENABLE_APLAY
bin_PROGRAMS += bluealsa-aplay
bluealsa_aplay_SOURCES = \
	aplay.c
bluealsa_aplay_CFLAGS = \
	@ALSA_CFLAGS@ \
	@BLUEZ_CFLAGS@ \
	@GIO2_CFLAGS@ \
	$(LOCAL_BLUEALSA_INCLUDES)
bluealsa_aplay_LDADD = \
	@ALSA_LIBS@ \
	@BLUEZ_LIBS@ \
	@GIO2_LIBS@ \
	../src/libbluealsa.la
endif

if ENABLE_RFCOMM
bin_PROGRAMS += bluealsa-rfcomm
bluealsa_rfcomm_SOURCES = \
	rfcomm.c
bluealsa_rfcomm_CFLAGS = \
	-I$(top_srcdir)/src \
	$(LOCAL_BLUEALSA_INCLUDES) \
	@BLUEZ_CFLAGS@
bluealsa_rfcomm_LDADD = \
	@BLUEZ_LIBS@ \
	-lreadline \
	../src/libbluealsa.la
endif

if ENABLE_HCITOP
bin_PROGRAMS += hcitop
hcitop_CFLAGS = \
	@BLUEZ_CFLAGS@ \
	@LIBBSD_CFLAGS@ \
	@NCURSES_CFLAGS@
hcitop_LDADD = \
	@BLUEZ_LIBS@ \
	@LIBBSD_LIBS@ \
	@NCURSES_LIBS@
endif
