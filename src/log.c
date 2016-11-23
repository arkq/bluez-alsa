/*
 * BlueALSA - log.c
 * Copyright (c) 2016 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "log.h"

#include <pthread.h>
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

	int oldstate;

	/* Threads cancellation is used extensively in the BlueALSA code. In order
	 * to prevent termination within the logging function (which might provide
	 * important information about what has happened), the thread cancellation
	 * has to be temporally disabled. */
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldstate);

	if (_syslog)
		vsyslog(priority, format, ap);

	flockfile(stderr);

	if (_ident != NULL)
		fprintf(stderr, "%s: ", _ident);
	vfprintf(stderr, format, ap);
	fputs("\n", stderr);

	funlockfile(stderr);

	pthread_setcancelstate(oldstate, NULL);

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
