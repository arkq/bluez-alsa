# BlueALSA - Makefile.am
# SPDX-FileCopyrightText: 2016-2026 BlueALSA developers
# SPDX-License-Identifier: MIT

check_PROGRAMS = bluealsad-mock
check_LTLIBRARIES = libservices.la

BUILT_SOURCES = \
	dbus-ifaces.h \
	dbus-ifaces.c

MOSTLYCLEANFILES = \
	$(BUILT_SOURCES)

AM_CFLAGS = \
	-I$(top_srcdir)/src \
	-I$(top_srcdir)/test \
	-I$(top_builddir)/src \
	@BLUEZ_CFLAGS@ \
	@GIO2_CFLAGS@ \
	@GLIB2_CFLAGS@ \
	-static

bluealsad_mock_SOURCES = \
	mock-bluealsa.c \
	mock.c

bluealsad_mock_LDADD = \
	../../src/shared/libshared.la \
	../../src/libbluealsad.la \
	libservices.la \
	@BLUEZ_LIBS@ \
	@GIO2_LIBS@ \
	@GLIB2_LIBS@

libservices_la_SOURCES = \
	dbus-ifaces.xml \
	service-bluez.c \
	service-ofono.c \
	service-upower.c \
	service.c

libservices_la_LIBADD = \
	../../src/shared/libshared.la \
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
