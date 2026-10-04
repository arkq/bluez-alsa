# BlueALSA - Makefile.am
# SPDX-FileCopyrightText: 2016-2026 BlueALSA developers
# SPDX-License-Identifier: MIT

dbusifacedir = @DBUS_IFACE_DIR@
dist_dbusiface_DATA = org.bluealsa.xml

CODEGEN_DEFS =

if ENABLE_ASHA
CODEGEN_DEFS += --define=ENABLE_GATT
endif

if ENABLE_MIDI
CODEGEN_DEFS += --define=ENABLE_GATT
endif

LOCAL_TARGETS = \
	org.bluealsa.h \
	org.bluealsa.c \
	org.bluez.h \
	org.bluez.c \
	org.ofono.h \
	org.ofono.c

MOSTLYCLEANFILES = $(LOCAL_TARGETS)

all-local: $(LOCAL_TARGETS)

.xml.h:
	$(srcdir)/dbus-codegen.py --output $@ $(CODEGEN_DEFS) \
		--interface-info-header --interface-skeleton-header $<

.xml.c:
	$(srcdir)/dbus-codegen.py --output $@ $(CODEGEN_DEFS) \
		--interface-info-body --interface-skeleton-body $<
