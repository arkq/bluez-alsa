# BlueALSA - Makefile.am
# Copyright (c) 2016-2021 Arkadiusz Bokowy

systemdbluealsadargs = $(SYSTEMD_BLUEALSAD_ARGS)
systemdbluealsaaplayargs = $(SYSTEMD_BLUEALSA_APLAY_ARGS)
bluealsaduser = $(BLUEALSAD_USER)
bluealsaaplayuser = $(BLUEALSA_APLAY_USER)

systemdsystemunitdir = $(SYSTEMD_SYSTEM_UNIT_DIR)

dist_systemdsystemunit_DATA = \
	bluealsa-aplay.service \
	bluealsa.service

SUFFIXES = .in
MOSTLYCLEANFILES = $(dist_systemdsystemunit_DATA)

SYSTEMD_SERVICE_SUBS = \
	s,[@]bindir[@],$(bindir),g; \
	s,[@]systemdbluealsadargs[@],$(systemdbluealsadargs),g; \
	s,[@]systemdbluealsaaplayargs[@],$(systemdbluealsaaplayargs),g; \
	s,[@]bluealsaduser[@],$(bluealsaduser),g; \
	s,[@]bluealsaaplayuser[@],$(bluealsaaplayuser),g;

.in:
	$(SED) -e '$(SYSTEMD_SERVICE_SUBS)' < $< > $@
