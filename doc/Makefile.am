# BlueALSA - Makefile.am
# Copyright (c) 2016-2020 Arkadiusz Bokowy

man1_MANS = bluealsa.man bluealsa-aplay.man

SUFFIXES = -manpage.md .man
MOSTLYCLEANFILES = bluealsa.man bluealsa-aplay.man

-manpage.md.man:
	$(PANDOC) -s -t man -f markdown -o $@ $<
