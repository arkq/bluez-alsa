/*
 * BlueALSA - log.c
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "log.h"

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#if WITH_LIBUNWIND
# define UNW_LOCAL_ONLY
# include <libunwind.h>
#elif HAVE_EXECINFO_H
# include <execinfo.h>
#endif

#include "defs.h"
#include "rt.h"

/* internal logging identifier */
static char *_ident = NULL;
/* if true, system logging is enabled */
static bool _syslog = false;
/* minimum priority to be logged */
static int _priority_limit = LOG_DEBUG;

#if DEBUG_TIME

/* point "zero" for relative time */
static struct timespec _ts0;

__attribute__ ((constructor))
static void init_ts0(void) {
	gettimestamp(&_ts0);
}

#endif

void log_open(const char *ident, bool syslog) {

	if (ident != NULL)
		_ident = strdup(ident);

	if ((_syslog = syslog) == true)
		openlog(ident, 0, LOG_USER);

}

void log_set_min_priority(int priority) {
	_priority_limit = priority;
}

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

static void vlog(int priority, const char *format, va_list ap) {

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
	else {

#if DEBUG_TIME
		struct timespec ts;
		gettimestamp(&ts);
		timespecsub(&ts, &_ts0, &ts);
#endif

		flockfile(stderr);

		if (_ident != NULL)
			fprintf(stderr, "%s: ", _ident);

#if DEBUG_TIME
		fprintf(stderr, "%lu.%.6lu: ", (long int)ts.tv_sec, ts.tv_nsec / 1000);
#endif

#if DEBUG && HAVE_GETTID
		fprintf(stderr, "[%d] ", gettid());
#endif

		fprintf(stderr, "%s: ", priority2str[priority]);
		vfprintf(stderr, format, ap);
		fputs("\n", stderr);

		funlockfile(stderr);

	}

	pthread_setcancelstate(oldstate, NULL);

}

void log_message(int priority, const char *format, ...) {
	if (priority > _priority_limit)
		return;
	va_list ap;
	va_start(ap, format);
	vlog(priority, format, ap);
	va_end(ap);
}

#if DEBUG
/**
 * Dump current thread's call stack. */
void callstackdump_(const char *label) {

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

	for (size_t i = 1; i < n; i++)
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
 * @param data Address of the memory block.
 * @param len Number of bytes which should be printed. */
void hexdump_(const char *label, const void *data, size_t len) {

	char *buf = malloc(len * 3 + 1);
	char *p = buf;

	for (size_t i = 0; i < len; i++) {
		p += sprintf(p, "%02x", *(unsigned char *)data & 0xFF);
		data = ((unsigned char *)data) + 1;
	}

	*p = '\0';
	log_message(LOG_DEBUG, "%s [len=%zu]: %s", label, len, buf);
	free(buf);
}
#endif
