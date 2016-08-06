/*
 * bluealsa - log.c
 * Copyright (c) 2016 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>


/* internal logging identifier */
static char *_ident = NULL;
/* if true, system logging is enabled */
static int _syslog = 0;


void log_open(const char *ident, int syslog) {

	if ((_syslog = syslog) != 0)
		openlog(ident, 0, LOG_USER);

	free(_ident);
	_ident = strdup(ident);

}

static void vlog(int priority, const char *format, va_list ap) {

	if (_syslog)
		vsyslog(priority, format, ap);

	flockfile(stderr);

	if (_ident != NULL)
		fprintf(stderr, "%s: ", _ident);
	vfprintf(stderr, format, ap);
	fputs("\n", stderr);

	funlockfile(stderr);

}

void error(const char *format, ...) {
	va_list ap;
	va_start(ap, format);
	vlog(LOG_ERR, format, ap);
	va_end(ap);
}

void warn(const char *format, ...) {
	va_list ap;
	va_start(ap, format);
	vlog(LOG_WARNING, format, ap);
	va_end(ap);
}

void info(const char *format, ...) {
	va_list ap;
	va_start(ap, format);
	vlog(LOG_INFO, format, ap);
	va_end(ap);
}

void _debug(const char *format, ...) {
	va_list ap;
	va_start(ap, format);
	vlog(LOG_DEBUG, format, ap);
	va_end(ap);
}
