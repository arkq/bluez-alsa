# BlueALSA - Makefile.am
# Copyright (c) 2016-2020 Arkadiusz Bokowy

man1_MANS =
man8_MANS = bluealsa.man

if ENABLE_APLAY
man1_MANS += bluealsa-aplay.man
endif

if ENABLE_RFCOMM
man1_MANS += bluealsa-rfcomm.man
endif

SUFFIXES = -manpage.md .man
MOSTLYCLEANFILES = $(man1_MANS) $(man8_MANS)

-manpage.md.man:
	$(SED) 's/\$$VERSION\$$/@PACKAGE_STRING@/' $< | $(PANDOC) -s -f markdown -t man -o $@
