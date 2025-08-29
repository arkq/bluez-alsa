# BlueALSA - Makefile.am
# Copyright (c) 2016-2025 Arkadiusz Bokowy

asound_module_ctl_LTLIBRARIES = libasound_module_ctl_bluealsa.la
asound_module_pcm_LTLIBRARIES = libasound_module_pcm_bluealsa.la
asound_module_conf_DATA = 20-bluealsa.conf

libasound_module_ctl_bluealsa_la_SOURCES = \
	bluealsa-ctl.c
libasound_module_pcm_bluealsa_la_SOURCES = \
	bluealsa-pcm.c

asound_module_ctldir = @ALSA_PLUGIN_DIR@
asound_module_pcmdir = @ALSA_PLUGIN_DIR@

AM_CFLAGS = \
	-I$(top_srcdir)/src \
	@ALSA_CFLAGS@ \
	@BLUEZ_CFLAGS@ \
	@DBUS1_CFLAGS@

AM_LDFLAGS = -module -avoid-version

libasound_module_ctl_bluealsa_la_LIBADD = \
	../shared/libshared.la \
	@ALSA_LIBS@ \
	@DBUS1_LIBS@
libasound_module_pcm_bluealsa_la_LIBADD = \
	../shared/libshared.la \
	@ALSA_LIBS@ \
	@DBUS1_LIBS@

SUFFIXES = .conf.in .conf
MOSTLYCLEANFILES = $(asound_module_conf_DATA)

.conf.in.conf:
	$(SED) -e '' < $< > $@

if ALSA_1_1_7
# For ALSA >= 1.1.7 install the bluealsa config file into the $datadir tree and
# create a symbolic link in the ALSA addon config directory.

ALSA_DATADIR = alsa/alsa.conf.d
asound_module_confdir = $(datadir)/$(ALSA_DATADIR)
install-conf-hook:
	$(MKDIR_P) $(DESTDIR)$(ALSA_CONF_DIR)
	cd $(DESTDIR)$(ALSA_CONF_DIR) &&                \
	    for i in $(asound_module_conf_DATA); do     \
	        $(RM) $$i;                              \
	        $(LN_S) $(asound_module_confdir)/$$i .; \
	    done

uninstall-conf-hook:
	-cd $(DESTDIR)$(ALSA_CONF_DIR) &&           \
	    for i in $(asound_module_conf_DATA); do \
	        $(RM) $$i;                          \
	    done
	-cd $(DESTDIR)$(datadir) && \
	    rmdir -p $(ALSA_DATADIR)

install-data-hook: install-conf-hook
uninstall-local: uninstall-conf-hook

else
# For ALSA < 1.1.7 install the bluealsa config file into the ALSA addon config
# directory.

asound_module_confdir = @ALSA_CONF_DIR@
endif
