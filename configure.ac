# bluez-alsa - configure.ac
# Copyright (c) 2016 Arkadiusz Bokowy

AC_INIT([bluez-alsa], [0.1.0], [arkadiusz.bokowy@gmail.com])
AM_INIT_AUTOMAKE([foreign -Wall -Werror])

AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIRS([m4])

AC_PROG_CC
AC_PROG_INSTALL
AM_PROG_AR
AM_PROG_CC_C_O
LT_INIT

# configuration should match ALSA library
AC_PREFIX_DEFAULT([/usr])
AC_SUBST([runstatedir], [/var/run])

# support for debugging
AC_ARG_ENABLE([debug],
	AS_HELP_STRING([--enable-debug], [enable debugging support]))
AM_CONDITIONAL([ENABLE_DEBUG], [test "x$enable_debug" = "xyes"])
AM_COND_IF([ENABLE_DEBUG], [
	AC_DEFINE([DEBUG], [1], [Define to 1 if the debugging is enabled])
])

AC_CHECK_LIB([pthread], [pthread_create],
	[], [AC_MSG_ERROR([pthread library not found])])
AC_CHECK_HEADERS([ortp/rtp.h],
	[], [AC_MSG_ERROR([ortp/rtp.h header not found])])

PKG_CHECK_MODULES([ALSA], [alsa])
PKG_CHECK_MODULES([BLUEZ], [bluez])
PKG_CHECK_MODULES([GLIB2], [glib-2.0 >= 2.16])
PKG_CHECK_MODULES([GIO20], [gio-unix-2.0])
PKG_CHECK_MODULES([SBC], [sbc >= 1.2])

AC_ARG_WITH([alsaplugindir],
	AS_HELP_STRING([--with-alsaplugindir=dir], [path where ALSA plugin files are stored]),
	[alsaplugindir="$withval"], [alsaplugindir="$libdir/alsa-lib"])
AC_ARG_WITH([alsadatadir],
	AS_HELP_STRING([--with-alsadatadir=dir], [directory containing ALSA data files]),
	[alsadatadir="$withval"], [alsadatadir="$datadir/alsa"])

test "x$prefix" = xNONE && prefix=$ac_default_prefix
test "x$exec_prefix" = xNONE && exec_prefix=$prefix

# TODO: Get rid of "ev(a|i)l" statements.
# TIP: Wizard-level Autotconf coder is needed.
eval alsadatadir="$alsadatadir"
eval alsadatadir="$alsadatadir"
eval alsaplugindir="$alsaplugindir"
eval alsaplugindir="$alsaplugindir"

AC_DEFINE_UNQUOTED([ALSA_DATA_DIR], "$alsadatadir", [Directory containing ALSA data files.])
AC_DEFINE_UNQUOTED([ALSA_PLUGIN_DIR], "$alsaplugindir", [Directory containing ALSA add-on modules.])
AC_DEFINE_UNQUOTED([RUN_STATE_DIR], "$runstatedir", [Path where run statuses are stored.])

AC_SUBST([ALSA_DATA_DIR], [$alsadatadir])
AC_SUBST([ALSA_PLUGIN_DIR], [$alsaplugindir])
AC_SUBST([RUN_STATE_DIR], [$runstatedir])

AC_CONFIG_FILES([
	Makefile
	src/Makefile
	src/alsa/Makefile])
AC_OUTPUT
