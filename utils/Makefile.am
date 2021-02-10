# BlueALSA - Makefile.am
# Copyright (c) 2016-2020 Arkadiusz Bokowy

SUBDIRS = aplay cli rfcomm

bin_PROGRAMS =

if ENABLE_A2DPCONF
bin_PROGRAMS += a2dpconf
a2dpconf_CFLAGS = \
	-I$(top_srcdir)/src
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

if ENABLE_BASH_COMPLETION
bashcompdir = $(BASH_COMPLETION_DIR)
bashcomp_DATA = bash-completion/bluealsa

EXTRA_DIST = $(bashcomp_DATA)

SYMLINKS = \
	a2dpconf \
	bluealsa-aplay \
	bluealsa-cli \
	bluealsa-rfcomm \
	hcitop

install-data-hook:
	cd $(DESTDIR)$(bashcompdir) && \
	for link in $(SYMLINKS); do ln -sf bluealsa $$link; done

uninstall-hook:
	cd $(DESTDIR)$(bashcompdir) && rm -f $(SYMLINKS)
endif
