# BlueALSA - Makefile.am
# SPDX-FileCopyrightText: 2016-2026 BlueALSA developers
# SPDX-License-Identifier: MIT

systemdbluealsadargs = $(SYSTEMD_BLUEALSAD_ARGS)
systemdbluealsaaplayargs = $(SYSTEMD_BLUEALSA_APLAY_ARGS)
bluealsaduser = $(BLUEALSAD_USER)
bluealsaaplayuser = $(BLUEALSA_APLAY_USER)

systemdsystemunitdir = $(SYSTEMD_SYSTEM_UNIT_DIR)

dist_systemdsystemunit_DATA = \
	bluealsa-aplay.service \
	bluealsa.service

systemdsysuserdir = $(SYSTEMD_SYSUSERS_DIR)
SYSUSERS_CONF_DEPS =

if WITH_SYSUSER_BLUEALSA
SYSUSERS_CONF_DEPS += bluealsad-sysusers.conf.in
dist_systemdsysuser_DATA = bluealsa.conf
endif
if ENABLE_APLAY
if WITH_SYSUSER_BLUEALSA_APLAY
SYSUSERS_CONF_DEPS += bluealsa-aplay-sysusers.conf.in
dist_systemdsysuser_DATA = bluealsa.conf
endif
endif

SUFFIXES = .in
MOSTLYCLEANFILES = \
	$(dist_systemdsysuser_DATA) \
	$(dist_systemdsystemunit_DATA) \
	bluealsa.conf.in

SYSTEMD_SERVICE_SUBS = \
	s,[@]bindir[@],$(bindir),g; \
	s,[@]systemdbluealsadargs[@],$(systemdbluealsadargs),g; \
	s,[@]systemdbluealsaaplayargs[@],$(systemdbluealsaaplayargs),g; \
	s,[@]bluealsaduser[@],$(bluealsaduser),g; \
	s,[@]bluealsaaplayuser[@],$(bluealsaaplayuser),g;

bluealsa.conf.in: $(SYSUSERS_CONF_DEPS)
	grep -v --no-filename '^#' $^ > $@

.in:
	$(SED) -e '$(SYSTEMD_SERVICE_SUBS)' < $< > $@
