# BlueALSA - configure.ac
# Copyright (c) 2016-2024 Arkadiusz Bokowy

AC_PREREQ([2.62])
AC_INIT([BlueALSA],
	[m4_normalize(esyscmd([test -d .git && git describe --always --dirty || echo v4.3.1]))],
	[arkadiusz.bokowy@gmail.com], [bluez-alsa], [https://github.com/arkq/bluez-alsa])
AM_INIT_AUTOMAKE([foreign subdir-objects -Wall -Werror])

AC_CONFIG_SRCDIR([src/ba-config.c])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIR([m4])

AC_USE_SYSTEM_EXTENSIONS
m4_version_prereq(2.70, [AC_PROG_CC], [AC_PROG_CC_C99])
AC_OPENMP
AC_PROG_INSTALL
AC_PROG_LN_S
AC_PROG_MKDIR_P
AM_PROG_AR
AM_PROG_CC_C_O
LT_INIT

# configuration should match ALSA library
AC_PREFIX_DEFAULT([/usr])

# testing presence of pkg-config
AC_MSG_CHECKING([pkg-config m4 macros])
if test m4_ifdef([PKG_CHECK_MODULES], [yes], [no]) = "yes"; then
	AC_MSG_RESULT([yes]);
else
	AC_MSG_RESULT([no]);
	AC_MSG_ERROR([pkg-config is required. See pkg-config.freedesktop.org])
fi

# support for debugging
AC_ARG_ENABLE([debug],
	AS_HELP_STRING([--enable-debug], [enable debugging support]))
AM_CONDITIONAL([ENABLE_DEBUG], [test "x$enable_debug" = "xyes"])
AM_COND_IF([ENABLE_DEBUG], [
	AC_CHECK_FUNCS([gettid])
	AC_CHECK_LIB([SegFault], [backtrace])
	AC_DEFINE([DEBUG], [1], [Define to 1 if the debugging is enabled.])
])
AC_ARG_ENABLE([debug-time],
	AS_HELP_STRING([--enable-debug-time], [enable debug timing support]))
AM_CONDITIONAL([ENABLE_DEBUG_TIME], [test "x$enable_debug_time" = "xyes"])
AM_COND_IF([ENABLE_DEBUG_TIME], [
	AC_DEFINE([DEBUG_TIME], [1], [Define to 1 if the debug timing is enabled.])
])

# embedded test coverage
AC_ARG_WITH([coverage],
	AS_HELP_STRING([--with-coverage], [use lcov for test coverage reporting]))
AM_CONDITIONAL([WITH_COVERAGE], [test "x$with_coverage" = "xyes"])
AM_COND_IF([WITH_COVERAGE], [
	AC_PATH_PROG([LCOV], [lcov])
	AS_IF([test "x$LCOV" = "x"], [AC_MSG_ERROR([[--with-coverage requires lcov]])])
	AC_PATH_PROG([GENHTML], [genhtml])
	AS_IF([test "x$GENHTML" = "x"], [AC_MSG_ERROR([[--with-coverage requires genhtml]])])
])

# in-place call-stack unwinding
AC_ARG_WITH([libunwind],
	AS_HELP_STRING([--with-libunwind], [use libunwind for call-stack unwinding]))
AM_CONDITIONAL([WITH_LIBUNWIND], [test "x$with_libunwind" = "xyes"])
AM_COND_IF([WITH_LIBUNWIND], [
	PKG_CHECK_MODULES([LIBUNWIND], [libunwind >= 1.1])
	AC_DEFINE([WITH_LIBUNWIND], [1], [Define to 1 if libunwind shall be used.])
], [
	AC_CHECK_HEADERS([execinfo.h])
])

AC_CHECK_HEADERS([stdatomic.h],
	[], [AC_MSG_ERROR([C11 atomic support not available])])
AC_CHECK_FUNCS([eventfd],
	[], [AC_MSG_ERROR([unable to find eventfd() function])])
AC_CHECK_FUNCS([pipe2],
	[], [AC_MSG_ERROR([unable to find pipe2() function])])
AC_CHECK_FUNCS([splice],
	[], [AC_MSG_ERROR([unable to find splice() function])])
AC_SEARCH_LIBS([clock_gettime], [rt],
	[], [AC_MSG_ERROR([unable to find clock_gettime() function])])
AC_SEARCH_LIBS([pow], [m],
	[], [AC_MSG_ERROR([unable to find pow() function])])
AC_SEARCH_LIBS([pthread_create], [pthread],
	[], [AC_MSG_ERROR([pthread library not found])])

PKG_CHECK_MODULES([ALSA], [alsa >= 1.0.27])
PKG_CHECK_MODULES([BLUEZ], [bluez >= 5.51])
PKG_CHECK_MODULES([DBUS1], [dbus-1 >= 1.6])
PKG_CHECK_MODULES([GIO2], [gio-unix-2.0])
PKG_CHECK_MODULES([GLIB2], [glib-2.0 >= 2.58.2])
PKG_CHECK_MODULES([SBC], [sbc >= 1.5])

AC_PATH_PROGS([GDBUS_CODEGEN], [gdbus-codegen])
AS_IF([test "x$GDBUS_CODEGEN" = "x"], [AC_MSG_ERROR([[gdbus-codegen not found]])])

PKG_CHECK_MODULES([LIBBSD], [libbsd >= 0.8],
	AC_DEFINE([HAVE_LIBBSD], [1], [Define to 1 if you have the libbsd library.]), [:])

AM_CONDITIONAL([ALSA_1_1_2__1_1_3], [$PKG_CONFIG "alsa >= 1.1.2 alsa <= 1.1.3"])
AM_CONDITIONAL([ALSA_1_1_7], [$PKG_CONFIG --atleast-version=1.1.7 alsa])

AC_ARG_ENABLE([aac],
	[AS_HELP_STRING([--enable-aac], [enable AAC support])])
AM_CONDITIONAL([ENABLE_AAC], [test "x$enable_aac" = "xyes"])
AM_COND_IF([ENABLE_AAC], [
	PKG_CHECK_MODULES([AAC], [fdk-aac >= 0.1.1])
	AC_DEFINE([ENABLE_AAC], [1], [Define to 1 if AAC is enabled.])
])

AC_ARG_WITH([libopenaptx],
	[AS_HELP_STRING([--with-libopenaptx], [use libopenaptx by pali.rohar for apt-X (HD)])])
AM_CONDITIONAL([WITH_LIBOPENAPTX], [test "x$with_libopenaptx" = "xyes"])

AC_ARG_WITH([libfreeaptx],
	[AS_HELP_STRING([--with-libfreeaptx], [use libfreeaptx for apt-X (HD)])])
AM_CONDITIONAL([WITH_LIBFREEAPTX], [test "x$with_libfreeaptx" = "xyes"])

# one of libopenaptx or libfreeaptx have been selected
AM_CONDITIONAL([WITH_lIBOPENAPTX_OR_WITH_LIBFREEAPTX],
	[test "x$with_libopenaptx" = "xyes" -o "x$with_libfreeaptx" = "xyes"])

# both libopenaptx and libfreeaptx have been selected
AM_CONDITIONAL([WITH_lIBOPENAPTX_AND_WITH_LIBFREEAPTX],
	[test "x$with_libopenaptx" = "xyes" -a "x$with_libfreeaptx" = "xyes"])
AM_COND_IF([WITH_lIBOPENAPTX_AND_WITH_LIBFREEAPTX],
	[AC_MSG_ERROR([select at most one of libopenaptx or libfreeaptx])])

AC_ARG_ENABLE([aptx],
	[AS_HELP_STRING([--enable-aptx], [enable apt-X support])])
AM_CONDITIONAL([ENABLE_APTX], [test "x$enable_aptx" = "xyes"])
AM_COND_IF([ENABLE_APTX], [
	AM_COND_IF([WITH_LIBOPENAPTX], [
		PKG_CHECK_MODULES([APTX], [libopenaptx >= 0.2.0])
		AC_DEFINE([HAVE_APTX_DECODE], [1], [Define to 1 if you have apt-X decode library.])
		AC_DEFINE([WITH_LIBOPENAPTX], [1], [Define to 1 if libopenaptx shall be used.])
	], [
		AM_COND_IF([WITH_LIBFREEAPTX], [
			PKG_CHECK_MODULES([APTX], [libfreeaptx >= 0.1.1])
			AC_DEFINE([HAVE_APTX_DECODE], [1], [Define to 1 if you have apt-X decode library.])
			AC_DEFINE([WITH_LIBFREEAPTX], [1], [Define to 1 if libfreeaptx shall be used.])
		], [
			PKG_CHECK_MODULES([APTX], [openaptx >= 2.0.0])
			if test "$($PKG_CONFIG --variable=aptxdecoder openaptx)" = "true"; then
				AC_DEFINE([HAVE_APTX_DECODE], [1], [Define to 1 if you have apt-X decode library.])
			fi
		])
	])
	AC_DEFINE([ENABLE_APTX], [1], [Define to 1 if apt-X is enabled.])
])

AC_ARG_ENABLE([aptx_hd],
	[AS_HELP_STRING([--enable-aptx-hd], [enable apt-X HD support])])
AM_CONDITIONAL([ENABLE_APTX_HD], [test "x$enable_aptx_hd" = "xyes"])
AM_COND_IF([ENABLE_APTX_HD], [
	AM_COND_IF([WITH_LIBOPENAPTX], [
		PKG_CHECK_MODULES([APTX_HD], [libopenaptx >= 0.2.0])
		AC_DEFINE([HAVE_APTX_HD_DECODE], [1], [Define to 1 if you have apt-X HD decode library.])
		AC_DEFINE([WITH_LIBOPENAPTX], [1], [Define to 1 if libopenaptx shall be used.])
	], [
		AM_COND_IF([WITH_LIBFREEAPTX], [
			PKG_CHECK_MODULES([APTX_HD], [libfreeaptx >= 0.1.1])
			AC_DEFINE([HAVE_APTX_HD_DECODE], [1], [Define to 1 if you have apt-X HD decode library.])
			AC_DEFINE([WITH_LIBFREEAPTX], [1], [Define to 1 if libfreeaptx shall be used.])
		], [
			PKG_CHECK_MODULES([APTX_HD], [openaptx >= 2.0.0])
			if test "$($PKG_CONFIG --variable=aptxdecoder openaptx)" = "true"; then
				AC_DEFINE([HAVE_APTX_HD_DECODE], [1], [Define to 1 if you have apt-X HD decode library.])
			fi
		])
	])
	AC_DEFINE([ENABLE_APTX_HD], [1], [Define to 1 if apt-X HD is enabled.])
])

# OR-ed conditional which can be used in the Makefile.am
AM_CONDITIONAL([ENABLE_APTX_OR_APTX_HD],
	[test "x$enable_aptx" = "xyes" -o "x$enable_aptx_hd" = "xyes"])

AC_ARG_ENABLE([faststream],
	[AS_HELP_STRING([--enable-faststream], [enable FastStream support])])
AM_CONDITIONAL([ENABLE_FASTSTREAM], [test "x$enable_faststream" = "xyes"])
AM_COND_IF([ENABLE_FASTSTREAM], [
	AC_DEFINE([ENABLE_FASTSTREAM], [1], [Define to 1 if FastStream is enabled.])
])

AC_ARG_ENABLE([lc3plus],
	[AS_HELP_STRING([--enable-lc3plus], [enable LC3plus support])])
AM_CONDITIONAL([ENABLE_LC3PLUS], [test "x$enable_lc3plus" = "xyes"])
AM_COND_IF([ENABLE_LC3PLUS], [
	AC_CHECK_HEADERS([lc3plus.h],
		[], [AC_MSG_ERROR([LC3plus header file not found])])
	# There is no pkg-config support for LC3plus, so we have to check the library
	# version manually. We will check for the symbols that are available in the
	# v1.4.1 API. If the library is older the check will fail.
	AC_CHECK_LIB([LC3plus], [lc3plus_version, lc3plus_enc_set_frame_dms],
		[AC_SUBST([LC3PLUS_LIBS], [-lLC3plus])], [AC_MSG_ERROR([LC3plus library not found])])
	AC_DEFINE([ENABLE_LC3PLUS], [1], [Define to 1 if LC3plus is enabled.])
])

AC_ARG_ENABLE([lc3-swb],
	[AS_HELP_STRING([--enable-lc3-swb], [enable LC3-SWB support])])
AM_CONDITIONAL([ENABLE_LC3_SWB], [test "x$enable_lc3_swb" = "xyes"])
AM_COND_IF([ENABLE_LC3_SWB], [
	PKG_CHECK_MODULES([LC3], [lc3 >= 1.0.0])
	AC_DEFINE([ENABLE_HFP_CODEC_SELECTION], [1],
		[Define to 1 if HFP codec selection is enabled.])
	AC_DEFINE([ENABLE_LC3_SWB], [1], [Define to 1 if LC3-SWB is enabled.])
])

AC_ARG_ENABLE([ldac],
	[AS_HELP_STRING([--enable-ldac], [enable LDAC support])])
AM_CONDITIONAL([ENABLE_LDAC], [test "x$enable_ldac" = "xyes"])
AM_COND_IF([ENABLE_LDAC], [
	PKG_CHECK_MODULES([LDAC_ABR], [ldacBT-abr >= 1.0.0])
	PKG_CHECK_MODULES([LDAC_DEC], [ldacBT-dec >= 2.0.0],
		AC_DEFINE([HAVE_LDAC_DECODE], [1], [Define to 1 if you have LDAC decode module.]), [:])
	PKG_CHECK_MODULES([LDAC_ENC], [ldacBT-enc >= 2.0.0])
	AC_DEFINE([ENABLE_LDAC], [1], [Define to 1 if LDAC is enabled.])
])

AC_ARG_ENABLE([lhdc],
	[AS_HELP_STRING([--enable-lhdc], [enable LHDC support])])
AM_CONDITIONAL([ENABLE_LHDC], [test "x$enable_lhdc" = "xyes"])
AM_COND_IF([ENABLE_LHDC], [
	AC_DEFINE([ENABLE_LHDC], [1], [Define to 1 if LHDC is enabled.])
	PKG_CHECK_MODULES([LHDC_DEC], [lhdcBT-dec >= 4.0.2])
	PKG_CHECK_MODULES([LHDC_ENC], [lhdcBT-enc >= 4.0.6])
])

AC_ARG_ENABLE([midi],
	[AS_HELP_STRING([--enable-midi], [enable Bluetooth LE MIDI support])])
AM_CONDITIONAL([ENABLE_MIDI], [test "x$enable_midi" = "xyes"])
AM_COND_IF([ENABLE_MIDI], [
	AC_DEFINE([ENABLE_MIDI], [1], [Define to 1 if Bluetooth LE MIDI is enabled.])
])

AC_ARG_ENABLE([mp3lame],
	[AS_HELP_STRING([--enable-mp3lame], [enable MP3 support])])
AM_CONDITIONAL([ENABLE_MP3LAME], [test "x$enable_mp3lame" = "xyes"])
AM_COND_IF([ENABLE_MP3LAME], [
	PKG_CHECK_MODULES([MP3LAME], [lame >= 3.100],,
		# The pkg-config support is available in the not yet released version of
		# LAME, so for now we have to check for the library manually as well to
		# be compatible with most Linux distributions.
		AC_CHECK_LIB([mp3lame], [lame_init],
			[AC_SUBST([MP3LAME_LIBS], [-lmp3lame])], [AC_MSG_ERROR([mp3lame library not found])]))
	AC_DEFINE([ENABLE_MPEG], [1], [Define to 1 if MPEG is enabled.])
	AC_DEFINE([ENABLE_MP3LAME], [1], [Define to 1 if MP3LAME is enabled.])
])

AC_ARG_ENABLE([mpg123],
	[AS_HELP_STRING([--enable-mpg123], [enable MPG123 decoding support])])
AM_CONDITIONAL([ENABLE_MPG123], [test "x$enable_mpg123" = "xyes"])
AM_COND_IF([ENABLE_MPG123], [
	PKG_CHECK_MODULES([MPG123], [libmpg123 >= 1.0.0])
	AC_DEFINE([ENABLE_MPEG], [1], [Define to 1 if MPEG is enabled.])
	AC_DEFINE([ENABLE_MPG123], [1], [Define to 1 if MPG123 is enabled.])
])

# OR-ed conditional which can be used in the Makefile.am
AM_CONDITIONAL([ENABLE_MPEG], [
	test "x$enable_mp3lame" = "xyes" -o "x$enable_mpg123" = "xyes"])

AC_ARG_ENABLE([msbc],
	[AS_HELP_STRING([--enable-msbc], [enable mSBC support])])
AM_CONDITIONAL([ENABLE_MSBC], [test "x$enable_msbc" = "xyes"])
AM_COND_IF([ENABLE_MSBC], [
	PKG_CHECK_MODULES([SPANDSP], [spandsp >= 0.0.6])
	AC_DEFINE([ENABLE_HFP_CODEC_SELECTION], [1],
		[Define to 1 if HFP codec selection is enabled.])
	AC_DEFINE([ENABLE_MSBC], [1], [Define to 1 if mSBC is enabled.])
])

AC_ARG_ENABLE([opus],
	AS_HELP_STRING([--enable-opus], [enable Opus support]))
AM_CONDITIONAL([ENABLE_OPUS], [test "x$enable_opus" = "xyes"])
AM_COND_IF([ENABLE_OPUS], [
	PKG_CHECK_MODULES([OPUS], [opus >= 1.0.0])
	AC_DEFINE([ENABLE_OPUS], [1], [Define to 1 if Opus is enabled.])
])

AC_ARG_ENABLE([ofono],
	AS_HELP_STRING([--enable-ofono], [enable HFP over oFono]))
AM_CONDITIONAL([ENABLE_OFONO], [test "x$enable_ofono" = "xyes"])
AM_COND_IF([ENABLE_OFONO], [
	AC_DEFINE([ENABLE_OFONO], [1], [Define to 1 if oFono is enabled.])
])

AC_ARG_ENABLE([systemd],
	AS_HELP_STRING([--enable-systemd], [enable systemd integration]))
AM_CONDITIONAL([ENABLE_SYSTEMD], [test "x$enable_systemd" = "xyes"])
AM_COND_IF([ENABLE_SYSTEMD], [
	AC_DEFINE([ENABLE_SYSTEMD], [1], [Define to 1 if systemd is enabled.])
])

AC_ARG_ENABLE([upower],
	AS_HELP_STRING([--enable-upower], [enable UPower integration]))
AM_CONDITIONAL([ENABLE_UPOWER], [test "x$enable_upower" = "xyes"])
AM_COND_IF([ENABLE_UPOWER], [
	AC_DEFINE([ENABLE_UPOWER], [1], [Define to 1 if UPower is enabled.])
])

AC_ARG_ENABLE([payloadcheck],
	[AS_HELP_STRING([--disable-payloadcheck], [disable RTP payload type check (workaround
		for PulseAudio < 13.0 bug)])])
AM_CONDITIONAL([ENABLE_PAYLOADCHECK], [test "x$enable_payloadcheck" != "xno"])
AM_COND_IF([ENABLE_PAYLOADCHECK], [
	AC_DEFINE([ENABLE_PAYLOADCHECK], [1], [Define to 1 if PAYLOADCHECK is enabled.])
])

AC_ARG_ENABLE([ctl],
	[AS_HELP_STRING([--disable-ctl], [disable building of bluealsactl tool])])
AM_CONDITIONAL([ENABLE_CTL], [test "x$enable_ctl" != "xno"])

AC_ARG_ENABLE([aplay],
	[AS_HELP_STRING([--disable-aplay], [disable building of bluealsa-aplay tool])])
AM_CONDITIONAL([ENABLE_APLAY], [test "x$enable_aplay" != "xno"])

AC_ARG_WITH([libsamplerate],
	[AS_HELP_STRING([--with-libsamplerate], [use libsamplerate in bluealsa-aplay])])
AM_CONDITIONAL([WITH_LIBSAMPLERATE], [test "x$with_libsamplerate" = "xyes"])
AM_COND_IF([WITH_LIBSAMPLERATE], [
	PKG_CHECK_MODULES([SAMPLERATE], [samplerate >= 0.2.0])
	AC_DEFINE([WITH_LIBSAMPLERATE], [1], [Define to 1 if libsamplerate shall be used.])
])

AC_ARG_ENABLE([rfcomm],
	[AS_HELP_STRING([--enable-rfcomm], [enable building of bluealsa-rfcomm tool])])
AM_CONDITIONAL([ENABLE_RFCOMM], [test "x$enable_rfcomm" = "xyes"])
AM_COND_IF([ENABLE_RFCOMM], [
	PKG_CHECK_MODULES([READLINE], [readline])
])

AC_ARG_ENABLE([a2dpconf],
	[AS_HELP_STRING([--enable-a2dpconf], [enable building of a2dpconf tool])])
AM_CONDITIONAL([ENABLE_A2DPCONF], [test "x$enable_a2dpconf" = "xyes"])

AC_ARG_ENABLE([hcitop],
	[AS_HELP_STRING([--enable-hcitop], [enable building of hcitop tool])])
AM_CONDITIONAL([ENABLE_HCITOP], [test "x$enable_hcitop" = "xyes"])
AM_COND_IF([ENABLE_HCITOP], [
	PKG_CHECK_MODULES([LIBBSD], [libbsd >= 0.8])
	PKG_CHECK_MODULES([NCURSES], [ncurses])
])

AC_ARG_ENABLE([manpages],
	AS_HELP_STRING([--enable-manpages], [enable building of man pages (requires rst2man)]))
AM_CONDITIONAL([ENABLE_MANPAGES], [test "x$enable_manpages" = "xyes"])
AM_COND_IF([ENABLE_MANPAGES], [
	AC_PATH_PROGS([RST2MAN], [rst2man rst2man.py])
	AS_IF([test "x$RST2MAN" = "x"], [AC_MSG_ERROR([[--enable-manpages requires rst2man]])])
])

AC_ARG_ENABLE([test],
	[AS_HELP_STRING([--enable-test], [enable unit test])])
AM_CONDITIONAL([HAVE_SNDFILE], [false])
AM_CONDITIONAL([ENABLE_TEST], [test "x$enable_test" = "xyes"])
AM_COND_IF([ENABLE_TEST], [
	AC_SEARCH_LIBS([dlsym], [dl],
		[], [AC_MSG_ERROR([unable to find dlsym() function])])
	PKG_CHECK_MODULES([CHECK], [check >= 0.11.0])
	PKG_CHECK_MODULES([SNDFILE], [sndfile >= 1.0],
		AM_CONDITIONAL([HAVE_SNDFILE], [true])
		AC_DEFINE([HAVE_SNDFILE], [1], [Define to 1 if you have the sndfile library.]), [:])
])

AC_ARG_WITH([bash-completion],
	AS_HELP_STRING([--with-bash-completion@<:@=DIR@:>@], [install bash completion
		script in DIR (or the bash completions default directory if not specified)]))
AM_CONDITIONAL([WITH_BASH_COMPLETION],
	[test "x$with_bash_completion" != "x" -a "x$with_bash_completion" != "xno"])
AS_IF([test "x$with_bash_completion" = "xyes"],
	[PKG_CHECK_MODULES([BASH_COMPLETION], [bash-completion >= 2.0],
		[bashcompdir=$($PKG_CONFIG --variable=completionsdir bash-completion)],
		[bashcompdir="$datadir/bash-completion/completions"])],
	[bashcompdir="$with_bash_completion"])
AC_SUBST([BASH_COMPLETION_DIR], [$bashcompdir])

AC_ARG_WITH([alsaplugindir],
	AS_HELP_STRING([--with-alsaplugindir=DIR], [path where ALSA plugin files are stored]),
	AS_IF([test "x$withval" == "xyes" -o "x$withval" == "xno"],
		[AC_MSG_ERROR([Option --with-alsaplugindir requires an argument])],
		[alsaplugindir="$withval"]),
	[alsaplugindir=$($PKG_CONFIG --variable=libdir alsa)/alsa-lib])
AC_SUBST([ALSA_PLUGIN_DIR], [$alsaplugindir])

# If no --localstatedir and --prefix options were given, use the /var as a
# default value for the local state directory.
if test "$localstatedir" = "\${prefix}/var"; then
	test "x$prefix" = xNONE && AC_SUBST([localstatedir], [/var])
fi

AC_SUBST([localstoragedir], [$localstatedir/lib])
if test "$localstatedir" = "\${prefix}/var"; then
	AC_SUBST([localstoragedir], [$prefix/var/lib])
fi

# Unfortunately, for ALSA >= 1.1.7 the directory for add-on configuration files
# is hard-coded as /etc/alsa/conf.d (unless the distribution has patched the
# source codes). So, we will use that value as a default, unless (for backwards
# compatibility) the user overrides it with --prefix or --sysconfdir option.
if test "$sysconfdir" = "\${prefix}/etc"; then
	test "x$prefix" = xNONE && AC_SUBST([sysconfdir], [/etc])
fi

AC_DEFINE_UNQUOTED(BLUEALSA_STORAGE_DIR, "${localstoragedir}/bluealsa",
	[Directory for the storage files.])

AC_ARG_WITH([alsaconfdir],
	AS_HELP_STRING([--with-alsaconfdir=DIR], [path to ALSA add-on configuration files]),
	AS_IF([test "x$withval" == "xyes" -o "x$withval" == "xno"],
		[AC_MSG_ERROR([Option --with-alsaconfdir requires an argument])],
		[alsaconfdir="$withval"]),
	[AM_COND_IF([ALSA_1_1_7],
		[alsaconfdir="$sysconfdir/alsa/conf.d"],
		[alsaconfdir="$datadir/alsa/alsa.conf.d"])])
AC_SUBST([ALSA_CONF_DIR], [$alsaconfdir])

AC_ARG_WITH([dbusconfdir],
	AS_HELP_STRING([--with-dbusconfdir=DIR], [path to D-Bus system bus configuration files]),
	AS_IF([test "x$withval" == "xyes" -o "x$withval" == "xno"],
		[AC_MSG_ERROR([Option --with-dbusconfdir requires an argument])],
		[dbusconfdir="${withval}"]),
	[dbusconfdir=$($PKG_CONFIG --variable=datadir dbus-1)/dbus-1/system.d])
AC_SUBST([DBUS_CONF_DIR], [$dbusconfdir])

AC_ARG_WITH([systemdsystemunitdir],
	AS_HELP_STRING([--with-systemdsystemunitdir=DIR], [path to systemd system unit files]),
	AS_IF([test "x$withval" == "xyes" -o "x$withval" == "xno"],
		[AC_MSG_ERROR([Option --with-systemdsystemunitdir requires an argument])],
		[systemdsystemunitdir="${withval}"]),
	[AM_COND_IF([ENABLE_SYSTEMD],
		[systemdsystemunitdir=$($PKG_CONFIG --variable=systemdsystemunitdir systemd)])])
AC_SUBST([SYSTEMD_SYSTEM_UNIT_DIR], [$systemdsystemunitdir])

AC_ARG_WITH([systemdbluealsadargs],
	AS_HELP_STRING([--with-systemdbluealsadargs=ARGS], [bluealsad arguments to be used in
		bluealsa.service, defaults to '-S -p a2dp-source -p a2dp-sink --all-codecs' if not specified]),
	AS_IF([test "x$withval" == "xyes" -o "x$withval" == "xno"],
		[AC_MSG_ERROR([Option --with-systemdbluealsadargs requires an argument])],
		[systemdbluealsadargs="${withval}"]),
	[systemdbluealsadargs="-S -p a2dp-source -p a2dp-sink --all-codecs"])
AC_SUBST([SYSTEMD_BLUEALSAD_ARGS], [$systemdbluealsadargs])

AC_ARG_WITH([systemdbluealsaaplayargs],
	AS_HELP_STRING([--with-systemdbluealsaaplayargs=ARGS], [bluealsa-aplay arguments to
		be used in bluealsa-aplay.service, defaults to '-S' if not specified]),
	AS_IF([test "x$withval" == "xyes" -o "x$withval" == "xno"],
		[AC_MSG_ERROR([Option --with-systemdbluealsaaplayargs requires an argument])],
		[systemdbluealsaaplayargs="${withval}"]),
	[systemdbluealsaaplayargs="-S"])
AC_SUBST([SYSTEMD_BLUEALSA_APLAY_ARGS], [$systemdbluealsaaplayargs])

AC_ARG_WITH([bluealsaduser],
	AS_HELP_STRING([--with-bluealsaduser=USER], [set up installation to run bluealsad as user
		USER, defaults to root if not specified. When used with bluez <= 5.50, USER must be a
		member of the "bluetooth" group.]),
	AS_IF([test "x$withval" == "xyes" -o "x$withval" == "xno"],
		[AC_MSG_ERROR([Option --with-bluealsaduser requires an argument])],
		[bluealsaduser="${withval}"]),
	[bluealsaduser="root"])
AC_SUBST([BLUEALSAD_USER], [$bluealsaduser])

AC_ARG_WITH([bluealsaaplayuser],
	AS_HELP_STRING([--with-bluealsaaplayuser=USER], [set up installation to run bluealsa-aplay
		as user USER, defaults to root if not specified. USER must be a member of the "audio"
		group.]),
	AS_IF([test "x$withval" == "xyes" -o "x$withval" == "xno"],
		[AC_MSG_ERROR([Option --with-bluealsaaplayuser requires an argument])],
		[bluealsaaplayuser="${withval}"]),
	[bluealsaaplayuser="root"])
AC_SUBST([BLUEALSA_APLAY_USER], [$bluealsaaplayuser])

AC_CONFIG_FILES([
	Makefile
	doc/Makefile
	misc/Makefile
	misc/systemd/Makefile
	src/Makefile
	src/asound/Makefile
	src/bluealsactl/Makefile
	src/shared/Makefile
	utils/Makefile
	utils/aplay/Makefile
	utils/rfcomm/Makefile
	test/Makefile
	test/mock/Makefile])
AC_OUTPUT

# warn user that alsa-lib thread-safety makes troubles
AM_COND_IF([ALSA_1_1_2__1_1_3], [
	AC_MSG_WARN([         *** alsa-lib >= 1.1.2, <= 1.1.3 ***])
	AC_MSG_WARN([Starting from alsa-lib 1.1.2, it is possible to enable])
	AC_MSG_WARN([thread-safe API functions. Unfortunately, this feature])
	AC_MSG_WARN([is not mature enough - software plugins may experience])
	AC_MSG_WARN([random deadlocks (bluez-alsa PCM plugin is affected).])
	AC_MSG_WARN([Either compile alsa-lib without this feature or export])
	AC_MSG_WARN([LIBASOUND_THREAD_SAFE=0 while using bluealsa PCM.])
])

# notify user that using libopenaptx or libfreeaptx might be a good idea
AM_COND_IF([ENABLE_APTX_OR_APTX_HD], [
AM_COND_IF([WITH_lIBOPENAPTX_OR_WITH_LIBFREEAPTX], [], [
	AC_MSG_NOTICE([       *** using apt-X (HD) via Qualcomm API ***])
	AC_MSG_NOTICE([Enabled apt-X (HD) support requires encoder (and decoder)])
	AC_MSG_NOTICE([library with a proprietary Qualcomm API. Please consider])
	AC_MSG_NOTICE([using --with-libopenaptx or --with-libfreeaptx option])
	AC_MSG_NOTICE([which will use a library based on FFmpeg project with])
	AC_MSG_NOTICE([better decoding support.])
])])
