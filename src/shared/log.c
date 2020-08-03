/*
 * BlueALSA - log.c
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "shared/log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#if WITH_LIBUNWIND
# define UNW_LOCAL_ONLY
# include <libunwind.h>
#elif HAVE_EXECINFO_H
# include <execinfo.h>
#endif

#include "shared/defs.h"
#include "shared/rt.h"

/* internal logging identifier */
static char *_ident = NULL;
/* if true, system logging is enabled */
static bool _syslog = false;
/* if true, print logging time */
static bool _time = BLUEALSA_LOGTIME;

void log_open(const char *ident, bool syslog, bool time) {

	free(_ident);
	_ident = strdup(ident);

	if ((_syslog = syslog) == true)
		openlog(ident, 0, LOG_USER);

	_time = time;

}

static void vlog(int priority, const char *format, va_list ap) {

	static const char *priority2str[] = {
		[LOG_EMERG] = "X",
		[LOG_ALERT] = "A",
		[LOG_CRIT] = "C",
		[LOG_ERR] = "E",
		[LOG_WARNING] = "W",
		[LOG_NOTICE] = "N",
		[LOG_INFO] = "I",
		[LOG_DEBUG] = "D",
	};

	int oldstate;

	/* Threads cancellation is used extensively in the BlueALSA code. In order
	 * to prevent termination within the logging function (which might provide
	 * important information about what has happened), the thread cancellation
	 * has to be temporally disabled. */
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldstate);

	if (_syslog) {
		va_list ap_syslog;
		va_copy(ap_syslog, ap);
		vsyslog(priority, format, ap_syslog);
		va_end(ap_syslog);
	}

	flockfile(stderr);

	if (_ident != NULL)
		fprintf(stderr, "%s: ", _ident);
	if (_time) {
		struct timespec ts;
		gettimestamp(&ts);
		fprintf(stderr, "%lu.%.9lu: ", (long int)ts.tv_sec, ts.tv_nsec);
	}
	fprintf(stderr, "%s: ", priority2str[priority]);
	vfprintf(stderr, format, ap);
	fputs("\n", stderr);

	funlockfile(stderr);

	pthread_setcancelstate(oldstate, NULL);

}

void log_message(int priority, const char *format, ...) {
	va_list ap;
	va_start(ap, format);
	vlog(priority, format, ap);
	va_end(ap);
}

#if DEBUG
/**
 * Dump current thread's call stack. */
void callstackdump(const char *label) {

	char buffer[1024 * 2] = "Call stack backtrace not supported";
	char *ptr = buffer;

#if WITH_LIBUNWIND

	unw_cursor_t cursor;
	unw_context_t context;
	unw_word_t off;

	unw_getcontext(&context);
	unw_init_local(&cursor, &context);

	unw_step(&cursor);
	while (unw_step(&cursor)) {
		char symbol[256] = "";
		unw_get_proc_name(&cursor, symbol, sizeof(symbol), &off);
		ptr += snprintf(ptr, sizeof(buffer) + buffer - ptr, "%s+%#zx < ",
				symbol, off);
	}

	ptr[-3] = '\0';

#elif HAVE_EXECINFO_H

	void *frames[32];
	size_t n = backtrace(frames, ARRAYSIZE(frames));
	char **symbols = backtrace_symbols(frames, n);

	size_t i;
	for (i = 1; i < n; i++)
		ptr += snprintf(ptr, sizeof(buffer) + buffer - ptr, "%s%s",
				symbols[i], (i + 1 < n) ? " < " : "");

	free(symbols);

#endif

	log_message(LOG_DEBUG, "%s: %s", label, buffer);

}
#endif

#if DEBUG
/**
 * Dump memory using hexadecimal representation.
 *
 * @param label Label printed before the memory block output.
 * @param mem Address of the memory block.
 * @param len Number of bytes which should be printed. */
void hexdump(const char *label, const void *mem, size_t len) {

	char *buf = malloc(len * 3 + 1);
	char *p = buf;

	while (len--) {
		p += sprintf(p, " %02x", *(unsigned char *)mem & 0xFF);
		mem = ((unsigned char *)mem) + 1;
	}

	log_message(LOG_DEBUG, "%s:%s", label, buf);
	free(buf);
}
#endif
