/*
 * BlueALSA - rt.h
 * Copyright (c) 2016-2017 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_SHARED_RT_H_
#define BLUEALSA_SHARED_RT_H_

#include <time.h>

/**
 * Get system monotonic time-stamp.
 *
 * @param ts Address to the timespec structure where the time-stamp will
 *   be stored.
 * @return On success this function returns 0. Otherwise, -1 is returned
 *   and errno is set to indicate the error. */
#ifdef CLOCK_MONOTONIC_RAW
# define gettimestamp(ts) clock_gettime(CLOCK_MONOTONIC_RAW, ts)
#else
# define gettimestamp(ts) clock_gettime(CLOCK_MONOTONIC, ts)
#endif

int difftimespec(
		const struct timespec *ts1,
		const struct timespec *ts2,
		struct timespec *ts);

#endif
