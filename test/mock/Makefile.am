# BlueALSA - Makefile.am
# SPDX-FileCopyrightText: 2016-2025 BlueALSA developers
# SPDX-License-Identifier: MIT

check_PROGRAMS = \
	bluealsad-mock

BUILT_SOURCES = \
	dbus-ifaces.h \
	dbus-ifaces.c

MOSTLYCLEANFILES = \
	$(BUILT_SOURCES)

bluealsad_mock_SOURCES = \
	dbus-ifaces.xml \
	mock-bluealsa.c \
	mock-bluez.c \
	mock-ofono.c \
	mock-upower.c \
	mock.c

AM_CFLAGS = \
	-I$(top_srcdir)/src \
	-I$(top_srcdir)/test \
	-I$(top_builddir)/src \
	@BLUEZ_CFLAGS@ \
	@GIO2_CFLAGS@ \
	@GLIB2_CFLAGS@

bluealsad_mock_LDADD = \
	../../src/shared/libshared.la \
	../../src/libbluealsad.la \
	@BLUEZ_LIBS@ \
	@GIO2_LIBS@ \
	@GLIB2_LIBS@

.xml.h:
	gdbus-codegen --output $@ \
		--interface-prefix org \
		--c-namespace Mock \
		--c-generate-autocleanup all \
		--c-generate-object-manager \
		--header $<

.xml.c:
	gdbus-codegen --output $@ \
		--interface-prefix org \
		--c-namespace Mock \
		--c-generate-autocleanup all \
		--c-generate-object-manager \
		--body $<
