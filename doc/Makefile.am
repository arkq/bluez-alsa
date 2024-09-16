# BlueALSA - Makefile.am
# Copyright (c) 2016-2024 Arkadiusz Bokowy

man1_MANS =

man7_MANS = \
	bluealsa-plugins.7 \
	org.bluealsa.Manager1.7 \
	org.bluealsa.PCM1.7 \
	org.bluealsa.RFCOMM1.7

man8_MANS = bluealsad.8

if ENABLE_CTL
man1_MANS += bluealsactl.1
endif

if ENABLE_APLAY
man1_MANS += bluealsa-aplay.1
endif

if ENABLE_RFCOMM
man1_MANS += bluealsa-rfcomm.1
endif

if ENABLE_A2DPCONF
man1_MANS += a2dpconf.1
endif

if ENABLE_HCITOP
man1_MANS += hcitop.1
endif

SUFFIXES = .rst
MOSTLYCLEANFILES = $(man1_MANS) $(man7_MANS) $(man8_MANS)

.rst:
	$(SED) 's/\$$VERSION\$$/@PACKAGE_STRING@/' $< | $(RST2MAN) - $@
