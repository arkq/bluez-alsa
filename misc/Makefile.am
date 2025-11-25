# BlueALSA - Makefile.am
# SPDX-FileCopyrightText: 2016-2025 BlueALSA developers
# SPDX-License-Identifier: MIT

SUBDIRS =

if WITH_BASH_COMPLETION

bashcompdir = @BASH_COMPLETION_DIR@
dist_bashcomp_DATA = bash-completion/bluealsad

SYMLINKS = \
	a2dpconf \
	bluealsactl \
	bluealsa-aplay \
	bluealsa-rfcomm \
	hcitop

install-data-hook:
	$(MKDIR_P) $(DESTDIR)$(bashcompdir) && \
	cd $(DESTDIR)$(bashcompdir) && \
	for link in $(SYMLINKS); do rm -f $$link; $(LN_S) bluealsad $$link; done

uninstall-hook:
	cd $(DESTDIR)$(bashcompdir) && rm -f $(SYMLINKS)

endif

if ENABLE_SYSTEMD
SUBDIRS += systemd
endif
