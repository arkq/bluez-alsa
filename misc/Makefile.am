# BlueALSA - Makefile.am
# Copyright (c) 2016-2021 Arkadiusz Bokowy

if WITH_BASH_COMPLETION

bashcompdir = @BASH_COMPLETION_DIR@
dist_bashcomp_DATA = bash-completion/bluealsa

SYMLINKS = \
	a2dpconf \
	bluealsa-aplay \
	bluealsa-cli \
	bluealsa-rfcomm \
	hcitop

install-data-hook:
	$(MKDIR_P) $(DESTDIR)$(bashcompdir) && \
	cd $(DESTDIR)$(bashcompdir) && \
	for link in $(SYMLINKS); do rm -f $$link; $(LN_S) bluealsa $$link; done

uninstall-hook:
	cd $(DESTDIR)$(bashcompdir) && rm -f $(SYMLINKS)

endif
