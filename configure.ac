# BlueALSA - configure.ac
# Copyright (c) 2016-2018 Arkadiusz Bokowy

AC_PREREQ([2.59])
AC_INIT([bluez-alsa], [1.2.0], [arkadiusz.bokowy@gmail.com])
AM_INIT_AUTOMAKE([foreign subdir-objects -Wall -Werror])

AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIR([m4])

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
	AC_DEFINE([DEBUG], [1], [Define to 1 if the debugging is enabled.])
])
AC_ARG_ENABLE([debug-time],
	AS_HELP_STRING([--enable-debug-time], [enable debug timing support]))
AM_CONDITIONAL([ENABLE_DEBUG_TIME], [test "x$enable_debug_time" = "xyes"])
AM_COND_IF([ENABLE_DEBUG_TIME], [
	AC_DEFINE([DEBUG_TIME], [1], [Define to 1 if the debug timing is enabled.])
])
AC_CHECK_FUNCS([register_printf_specifier])

AC_CHECK_LIB([pthread], [pthread_create],
	[], [AC_MSG_ERROR([pthread library not found])])
AC_SEARCH_LIBS([clock_gettime], [rt],
	[], [AC_MSG_ERROR([unable to find clock_gettime() function])])
AC_SEARCH_LIBS([pow], [m],
	[], [AC_MSG_ERROR([unable to find pow() function])])

PKG_CHECK_MODULES([ALSA], [alsa])
PKG_CHECK_MODULES([BLUEZ], [bluez >= 5.0])
PKG_CHECK_MODULES([GLIB2], [glib-2.0 >= 2.26])
PKG_CHECK_MODULES([GIO2], [gio-unix-2.0])
PKG_CHECK_MODULES([SBC], [sbc >= 1.2])

AM_CONDITIONAL([ALSA_1_1_2], [$PKG_CONFIG --atleast-version=1.1.2 alsa])

AC_ARG_ENABLE([aac],
	[AS_HELP_STRING([--enable-aac], [enable AAC support])])
AM_CONDITIONAL([ENABLE_AAC], [test "x$enable_aac" = "xyes"])
AM_COND_IF([ENABLE_AAC], [
	PKG_CHECK_MODULES([AAC], [fdk-aac >= 0.1.1])
	AC_DEFINE([ENABLE_AAC], [1], [Define to 1 if AAC is enabled.])
])

AC_ARG_ENABLE([aptx],
	[AS_HELP_STRING([--enable-aptx], [enable apt-X support])])
AM_CONDITIONAL([ENABLE_APTX], [test "x$enable_aptx" = "xyes"])
AM_COND_IF([ENABLE_APTX], [
	PKG_CHECK_MODULES([APTX], [openaptx >= 1.0.0])
	AC_DEFINE([ENABLE_APTX], [1], [Define to 1 if apt-X is enabled.])
])

AC_ARG_ENABLE([payloadcheck],
	[AS_HELP_STRING([--disable-payloadcheck], [disable RTP payload type check (workaround for a PulseAudio bug)])])
AM_CONDITIONAL([ENABLE_PAYLOADCHECK], [test "x$enable_payloadcheck" != "xno"])
AM_COND_IF([ENABLE_PAYLOADCHECK], [
	AC_DEFINE([ENABLE_PAYLOADCHECK], [1], [Define to 1 if PAYLOADCHECK is enabled.])
])

AC_ARG_ENABLE([aplay],
	[AS_HELP_STRING([--disable-aplay], [disable building of bluealsa-aplay tool])])
AM_CONDITIONAL([ENABLE_APLAY], [test "x$enable_aplay" != "xno"])

AC_ARG_ENABLE([rfcomm],
	[AS_HELP_STRING([--disable-rfcomm], [disable building of bluealsa-rfcomm tool])])
AM_CONDITIONAL([ENABLE_RFCOMM], [test "x$enable_rfcomm" != "xno"])

AC_ARG_ENABLE([hcitop],
	[AS_HELP_STRING([--enable-hcitop], [enable building of hcitop tool])])
AM_CONDITIONAL([ENABLE_HCITOP], [test "x$enable_hcitop" = "xyes"])
AM_COND_IF([ENABLE_HCITOP], [
	PKG_CHECK_MODULES([LIBBSD], [libbsd])
	PKG_CHECK_MODULES([NCURSES], [ncurses])
])

# Since it is not possible to specify custom ALSA plugin directory, BlueALSA
# PCM plugin has to be installed prior to this test.
AC_ARG_ENABLE([pcm-test],
	[AS_HELP_STRING([--enable-pcm-test], [enable PCM plugin test])])
AM_CONDITIONAL([ENABLE_PCM_TEST], [test "x$enable_pcm_test" = "xyes"])

AC_ARG_WITH([alsaplugindir],
	AS_HELP_STRING([--with-alsaplugindir=dir], [path where ALSA plugin files are stored]),
	[alsaplugindir="$withval"], [alsaplugindir=$($PKG_CONFIG --variable=libdir alsa)/alsa-lib])
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
	src/asound/Makefile
	utils/Makefile
	test/Makefile])
AC_OUTPUT

# warn user that alsa-lib thread-safety makes troubles
AM_COND_IF([ALSA_1_1_2], [
	AC_MSG_WARN([              *** alsa-lib >= 1.1.2 ***])
	AC_MSG_WARN([Starting from alsa-lib 1.1.2, it is possible to enable])
	AC_MSG_WARN([thread-safe API functions. Unfortunately, this feature])
	AC_MSG_WARN([is not mature enough - software plugins may experience])
	AC_MSG_WARN([random deadlocks (bluez-alsa PCM plugin is affected).])
	AC_MSG_WARN([Either compile alsa-lib without this feature or export])
	AC_MSG_WARN([LIBASOUND_THREAD_SAFE=0 while using bluealsa PCM.])
])

# warn user that aptX support is not an open-source feature
AM_COND_IF([ENABLE_APTX], [
	AC_MSG_WARN([             *** aptX encoder support ***])
	AC_MSG_WARN([You have enabled support for aptX encoding. Note, that])
	AC_MSG_WARN([by default bluez-alsa will use openaptx library, which])
	AC_MSG_WARN([does NOT perform any encoding/decoding.])
])
