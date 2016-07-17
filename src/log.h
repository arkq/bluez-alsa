/*
 * bluealsa - log.h
 * Copyright (c) 2016 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_LOG_H_
#define BLUEALSA_LOG_H_

#if HAVE_CONFIG_H
# include "../config.h"
#endif

void log_open(const char *ident, int syslog);
void error(const char *format, ...) __attribute__ ((format(printf, 1, 2)));
void warn(const char *format, ...) __attribute__ ((format(printf, 1, 2)));
void info(const char *format, ...) __attribute__ ((format(printf, 1, 2)));

#if DEBUG
void _debug(const char *format, ...) __attribute__ ((format(printf, 1, 2)));
# define debug(M, ARGS...) _debug("%s:%d: " M, __FILE__, __LINE__, ## ARGS)
#else
# define debug(M, ARGS...) do {} while (0)
#endif

#endif
