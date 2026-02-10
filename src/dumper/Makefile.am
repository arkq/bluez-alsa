# BlueALSA - Makefile.am
# SPDX-FileCopyrightText: 2026 BlueALSA developers
# SPDX-License-Identifier: MIT

noinst_LTLIBRARIES = libbadumper.la

libbadumper_la_SOURCES = \
	dumper-io.c \
	dumper-utils.c \
	dumper.c

AM_CFLAGS = \
	-I$(top_srcdir)/src \
	@GLIB2_CFLAGS@ \
	-static

libbadumper_la_LIBADD = \
	../shared/libshared.la
