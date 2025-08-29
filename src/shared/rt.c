/*
 * BlueALSA - rt.h
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "rt.h"

#include <stdlib.h>
#include <sys/time.h>

/**
 * Initialize rate synchronization.
 *
 * @param asrs Pointer to the rate synchronization structure.
 * @param rate Synchronization sample rate. */
void asrsync_init(struct asrsync *asrs, unsigned int rate) {
	asrs->rate = rate;
	gettimestamp(&asrs->ts0);
	asrs->ts = asrs->ts0;
	asrs->frames = 0;
}

/**
 * Synchronize time with the sample rate.
 *
 * Notes:
 * 1. Time synchronization relies on the frame counter being linear.
 * 2. In order to prevent frame counter overflow (for more information see
 *   the asrsync structure definition), this counter should be initialized
 *   (zeroed) upon every transfer stop.
 *
 * @param asrs Pointer to the rate synchronization structure.
 * @param frames Number of frames since the last call to this function. */
void asrsync_sync(struct asrsync *asrs, unsigned int frames) {

	const unsigned int rate = asrs->rate;
	struct timespec ts_rate;
	struct timespec ts;

	asrs->frames += frames;
	frames = asrs->frames;

	ts_rate.tv_sec = frames / rate;
	ts_rate.tv_nsec = 1000000000L / rate * (frames % rate);

	gettimestamp(&ts);

	asrs->synced = false;
	/* maintain constant rate */
	timespecsub(&ts, &asrs->ts0, &ts);
	if (difftimespec(&ts, &ts_rate, &asrs->ts_idle) > 0) {
		nanosleep(&asrs->ts_idle, NULL);
		asrs->synced = true;
	}

	gettimestamp(&asrs->ts);

}

/**
 * Get the time duration in 1/10 of milliseconds since the last sync. */
unsigned int asrsync_get_dms_since_last_sync(const struct asrsync *asrs) {

	struct timespec ts;
	gettimestamp(&ts);

	timespecsub(&ts, &asrs->ts, &ts);
	return ts.tv_sec * (1000000 / 100) + ts.tv_nsec / (1000 * 100);
}

/**
 * Calculate time difference for two time points.
 *
 * @param ts1 Address to the timespec structure providing t1 time point.
 * @param ts2 Address to the timespec structure providing t2 time point.
 * @param ts Address to the timespec structure where the absolute time
 *   difference will be stored.
 * @return This function returns an integer less than, equal to, or greater
 *   than zero, if t2 time point is found to be, respectively, less than,
 *   equal to, or greater than the t1 time point.*/
int difftimespec(
		const struct timespec *ts1,
		const struct timespec *ts2,
		struct timespec *ts) {

	const struct timespec _ts1 = *ts1;
	const struct timespec _ts2 = *ts2;

	if (_ts1.tv_sec == _ts2.tv_sec) {
		ts->tv_sec = 0;
		ts->tv_nsec = labs(_ts2.tv_nsec - _ts1.tv_nsec);
		return _ts2.tv_nsec - _ts1.tv_nsec;
	}

	if (_ts1.tv_sec < _ts2.tv_sec) {
		timespecsub(&_ts2, &_ts1, ts);
		return 1;
	}

	timespecsub(&_ts1, &_ts2, ts);
	return -1;
}
