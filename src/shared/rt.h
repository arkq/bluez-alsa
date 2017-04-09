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

#include <stdint.h>
#include <time.h>

/**
 * Structure used for time synchronization.
 *
 * With the size of the frame counter being 32 bits, it is possible to track
 * up to ~24 hours, with the sampling rate of 48 kHz. If it is insufficient,
 * one can switch to 64 bits, which would suffice for 12 million years. */
struct asrsync {

	/* used sampling rate */
	unsigned int rate;
	/* reference time point */
	struct timespec ts0;

	/* time-stamp from the previous sync */
	struct timespec ts;
	/* transfered frames since ts0 */
	uint32_t frames;

	/* time spent outside of the sync function */
	struct timespec ts_busy;
	/* If the asrsync_sync() returns a positive value, then this variable
	 * contains an amount of time used for synchronization. Otherwise, it
	 * contains an overdue time - synchronization was not possible due to
	 * too much time spent outside of the sync function. */
	struct timespec ts_idle;

};

/**
 * Start (initialize) time synchronization.
 *
 * @param asrs Time synchronization structure.
 * @param sr Synchronization sampling rate. */
#define asrsync_init(asrs, sr) do { asrs.rate = sr; asrs.frames = 0; \
	gettimestamp(&asrs.ts0); asrs.ts = asrs.ts0; } while(0)

int asrsync_sync(struct asrsync *asr, unsigned int frames);

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
