# BlueALSA - Makefile.am
# SPDX-FileCopyrightText: 2016-2025 BlueALSA developers
# SPDX-License-Identifier: MIT

SUBDIRS = shared
# Make sure that shared library is built before anything else.
SUBDIRS += asound bluealsactl

bin_PROGRAMS = bluealsad
noinst_LTLIBRARIES = libbluealsad.la

dbusconfdir = @DBUS_CONF_DIR@
dbusifacedir = @DBUS_IFACE_DIR@
dbusbluealsauser = @BLUEALSAD_USER@
dist_dbusconf_DATA = org.bluealsa.conf
dist_dbusiface_DATA = dbus/org.bluealsa.xml

BUILT_SOURCES = \
	dbus/org.bluealsa.h \
	dbus/org.bluealsa.c \
	dbus/org.bluez.h \
	dbus/org.bluez.c \
	dbus/org.ofono.h \
	dbus/org.ofono.c

MOSTLYCLEANFILES = \
	$(dist_dbusconf_DATA) \
	$(BUILT_SOURCES)

bluealsad_SOURCES = \
	main.c

bluealsad_LDADD = \
	shared/libshared.la \
	libbluealsad.la

libbluealsad_la_SOURCES = \
	a2dp.c \
	a2dp-sbc.c \
	at.c \
	audio.c \
	ba-adapter.c \
	ba-config.c \
	ba-device.c \
	ba-rfcomm.c \
	ba-transport.c \
	ba-transport-pcm.c \
	bluealsa-dbus.c \
	bluez.c \
	codec-sbc.c \
	dbus.c \
	dbus/org.bluealsa.xml \
	dbus/org.bluez.xml \
	error.c \
	h2.c \
	hci.c \
	hfp.c \
	io.c \
	rtp.c \
	sco.c \
	sco-cvsd.c \
	storage.c \
	utils.c

if ENABLE_AAC
libbluealsad_la_SOURCES += \
	a2dp-aac.c
endif

if ENABLE_APTX
libbluealsad_la_SOURCES += \
	a2dp-aptx.c
endif

if ENABLE_APTX_HD
libbluealsad_la_SOURCES += \
	a2dp-aptx-hd.c
endif

if ENABLE_APTX_OR_APTX_HD
libbluealsad_la_SOURCES += \
	codec-aptx.c
endif

if ENABLE_ASHA
libbluealsad_la_SOURCES += \
	asha-g722.c \
	asha.c
endif

if ENABLE_FASTSTREAM
libbluealsad_la_SOURCES += \
	a2dp-faststream.c
endif

if ENABLE_LC3_SWB
libbluealsad_la_SOURCES += \
	codec-lc3-swb.c \
	sco-lc3-swb.c
endif

if ENABLE_LC3PLUS
libbluealsad_la_SOURCES += \
	a2dp-lc3plus.c
endif

if ENABLE_LDAC
libbluealsad_la_SOURCES += \
	a2dp-ldac.c
endif

if ENABLE_MIDI
libbluealsad_la_SOURCES += \
	ble-midi.c \
	bluez-midi.c \
	bt-advertising.c \
	bt-gatt.c \
	midi.c
endif

if ENABLE_LHDC
libbluealsad_la_SOURCES += \
	a2dp-lhdc.c
endif

if ENABLE_MPEG
libbluealsad_la_SOURCES += \
	a2dp-mpeg.c
endif

if ENABLE_MSBC
libbluealsad_la_SOURCES += \
	codec-msbc.c \
	sco-msbc.c
endif

if ENABLE_OPUS
libbluealsad_la_SOURCES += \
	a2dp-opus.c
endif

if ENABLE_OFONO
libbluealsad_la_SOURCES += \
	ofono.c \
	dbus/org.ofono.xml
endif

if ENABLE_UPOWER
libbluealsad_la_SOURCES += \
	upower.c
endif

AM_CFLAGS = \
	@AAC_CFLAGS@ \
	@ALSA_CFLAGS@ \
	@APTX_CFLAGS@ \
	@APTX_HD_CFLAGS@ \
	@BLUEZ_CFLAGS@ \
	@GIO2_CFLAGS@ \
	@GLIB2_CFLAGS@ \
	@LC3_CFLAGS@ \
	@LDAC_ABR_CFLAGS@ \
	@LDAC_DEC_CFLAGS@ \
	@LDAC_ENC_CFLAGS@ \
	@LHDC_DEC_CFLAGS@ \
	@LHDC_ENC_CFLAGS@ \
	@MP3LAME_CFLAGS@ \
	@MPG123_CFLAGS@ \
	@OPUS_CFLAGS@ \
	@SBC_CFLAGS@ \
	@SPANDSP_CFLAGS@ \
	-static

libbluealsad_la_LIBADD = \
	shared/libshared.la \
	@AAC_LIBS@ \
	@ALSA_LIBS@ \
	@APTX_HD_LIBS@ \
	@APTX_LIBS@ \
	@BLUEZ_LIBS@ \
	@GIO2_LIBS@ \
	@GLIB2_LIBS@ \
	@LC3_LIBS@ \
	@LC3PLUS_LIBS@ \
	@LDAC_ABR_LIBS@ \
	@LDAC_DEC_LIBS@ \
	@LDAC_ENC_LIBS@ \
	@LHDC_DEC_LIBS@ \
	@LHDC_ENC_LIBS@ \
	@MP3LAME_LIBS@ \
	@MPG123_LIBS@ \
	@OPUS_LIBS@ \
	@SBC_LIBS@ \
	@SPANDSP_LIBS@

SUFFIXES = .conf.in .conf

DBUSCONF_SUBS = \
	s,[@]bluealsaduser[@],$(dbusbluealsauser),g;

.conf.in.conf:
	$(SED) -e '$(DBUSCONF_SUBS)' < $< > $@

CODEGEN_DEFS =
if ENABLE_MIDI
CODEGEN_DEFS += --define=ENABLE_MIDI
endif

.xml.h:
	$(srcdir)/dbus/dbus-codegen.py --output $@ $(CODEGEN_DEFS) \
		--interface-info-header --interface-skeleton-header $<

.xml.c:
	$(srcdir)/dbus/dbus-codegen.py --output $@ $(CODEGEN_DEFS) \
		--interface-info-body --interface-skeleton-body $<
