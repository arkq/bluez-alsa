/*
 * BlueALSA - log.h
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_SHARED_LOG_H_
#define BLUEALSA_SHARED_LOG_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdbool.h>
#include <stddef.h>
#include <syslog.h>

#if DEBUG_TIME
# define BLUEALSA_LOGTIME true
#else
# define BLUEALSA_LOGTIME false
#endif

void log_open(const char *ident, bool syslog, bool time);
void log_message(int priority, const char *format, ...) __attribute__ ((format(printf, 2, 3)));

#if DEBUG
# define error(M, ...) log_message(LOG_ERR, "%s:%d: " M, __FILE__, __LINE__, ## __VA_ARGS__)
# define warn(M, ...) log_message(LOG_WARNING, "%s:%d: " M, __FILE__, __LINE__, ## __VA_ARGS__)
# define info(M, ...) log_message(LOG_INFO, "%s:%d: " M, __FILE__, __LINE__, ## __VA_ARGS__)
# define debug(M, ...) log_message(LOG_DEBUG, "%s:%d: " M, __FILE__, __LINE__, ## __VA_ARGS__)
#else
# define error(M, ...) log_message(LOG_ERR, M, ## __VA_ARGS__)
# define warn(M, ...) log_message(LOG_WARNING, M, ## __VA_ARGS__)
# define info(M, ...) log_message(LOG_INFO, M, ## __VA_ARGS__)
# define debug(M, ...) do {} while (0)
#endif

#if DEBUG
void callstackdump(const char *label);
#else
# define callstackdump(M) do {} while (0)
#endif

#if DEBUG
void hexdump(const char *label, const void *mem, size_t len);
#else
# define hexdump(A, M, L) do {} while (0)
#endif

#endif
