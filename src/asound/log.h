/*
 * BlueALSA - asound/log.h
 * SPDX-FileCopyrightText: 2016-2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BLUEALSA_ASOUND_LOG_H_
#define BLUEALSA_ASOUND_LOG_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <alsa/asoundlib.h>

#if SND_LIB_VERSION >= 0x01020F

#if ! DEBUG
# ifdef snd_debug
# undef snd_debug
# endif
# define snd_debug(...) do {} while (0)
#endif

#define snd_log_init() do {} while (0)

#else /* SND_LIB_VERSION < 0x01020F */

#include <errno.h>

#define BA_LOG_ERR   4
#define BA_LOG_WARN  3
#define BA_LOG_INFO  2
#define BA_LOG_DEBUG 1

/* Earlier releases of alsa-lib had no notion of message priority, all messages
 * are error messages. So we insert an additional label to indicate priority. */
#define snd_error(interface, ...) \
	snd_lib_error(__FILE__, __LINE__, __func__, 0, "[error] " __VA_ARGS__)
#define snd_errornum(interface, ...) \
	snd_lib_error(__FILE__, __LINE__, __func__, errno, "[error] " __VA_ARGS__)
#define snd_warn(interface, ...) do { \
		if (__snd_log_level <= BA_LOG_WARN) \
			snd_lib_error(__FILE__, __LINE__, __func__, 0, "[warning] " __VA_ARGS__); \
	} while (0)
#define snd_info(interface, ...) do { \
		if (__snd_log_level <= BA_LOG_INFO) \
			snd_lib_error(__FILE__, __LINE__, __func__, 0, "[info] " __VA_ARGS__); \
	} while (0)

#if DEBUG
# define snd_debug(interface, ...) do { \
		if (__snd_log_level <= BA_LOG_DEBUG) \
			snd_lib_error(__FILE__, __LINE__, __func__, 0, "[debug] " __VA_ARGS__); \
	} while (0)
#else
# define snd_debug(...) do {} while (0)
#endif

extern int __snd_log_level;

/**
 * Initialize logging subsystem. */
void snd_log_init(void);

#endif

#endif
