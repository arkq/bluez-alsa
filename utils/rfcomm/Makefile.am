# BlueALSA - Makefile.am
# SPDX-FileCopyrightText: 2016-2025 BlueALSA developers
# SPDX-License-Identifier: MIT

if ENABLE_RFCOMM

bin_PROGRAMS = bluealsa-rfcomm

bluealsa_rfcomm_SOURCES = \
	rfcomm.c

AM_CFLAGS = \
	-I$(top_srcdir)/src \
	@BLUEZ_CFLAGS@ \
	@DBUS1_CFLAGS@ \
	@READLINE_CFLAGS@

bluealsa_rfcomm_LDADD = \
	../../src/shared/libshared.la \
	@DBUS1_LIBS@ \
	@READLINE_LIBS@

endif
