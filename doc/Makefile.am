# BlueALSA - Makefile.am
# Copyright (c) 2016-2020 Arkadiusz Bokowy

man1_MANS = bluealsa.man

if ENABLE_APLAY
man1_MANS += bluealsa-aplay.man
endif

SUFFIXES = -manpage.md .man
MOSTLYCLEANFILES = $(man1_MANS)

-manpage.md.man:
	$(PANDOC) -s -t man -f markdown -o $@ $<
