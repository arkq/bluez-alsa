/*
 * BlueALSA - rt.h
 * Copyright (c) 2016-2017 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "shared/rt.h"

#include <stdlib.h>


/**
 * Synchronize time with the sampling rate.
 *
 * Notes:
 * 1. Time synchronization relies on the frame counter being linear.
 * 2. In order to prevent frame counter overflow (for more information see
 *   the asrsync structure definition), this counter should be initialized
 *   (zeroed) upon every transfer stop.
 *
 * @param asrs Pointer to the time synchronization structure.
 * @param frames Number of frames since the last call to this function.
 * @return This function returns a positive value or zero respectively for
 *   the case, when the synchronization was required or when blocking was
 *   not necessary. If an error has occurred, -1 is returned and errno is
 *   set to indicate the error. */
int asrsync_sync(struct asrsync *asrs, unsigned int frames) {

	const unsigned int rate = asrs->rate;
	struct timespec ts_rate;
	struct timespec ts;
	int rv = 0;

	asrs->frames += frames;
	frames = asrs->frames;

	ts_rate.tv_sec = frames / rate;
	ts_rate.tv_nsec = 1000000000 / rate * (frames % rate);

	gettimestamp(&ts);
	/* calculate delay since the last sync */
	difftimespec(&asrs->ts, &ts, &asrs->ts_busy);

	/* maintain constant rate */
	difftimespec(&asrs->ts0, &ts, &ts);
	if (difftimespec(&ts, &ts_rate, &asrs->ts_idle) > 0) {
		nanosleep(&asrs->ts_idle, NULL);
		rv = 1;
	}

	gettimestamp(&asrs->ts);
	return rv;
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
		return _ts2.tv_nsec > _ts1.tv_nsec ? 1 : -ts->tv_nsec;
	}

	if (_ts1.tv_sec < _ts2.tv_sec) {
		if (_ts1.tv_nsec <= _ts2.tv_nsec) {
			ts->tv_sec = _ts2.tv_sec - _ts1.tv_sec;
			ts->tv_nsec = _ts2.tv_nsec - _ts1.tv_nsec;
		}
		else {
			ts->tv_sec = _ts2.tv_sec - 1 - _ts1.tv_sec;
			ts->tv_nsec = _ts2.tv_nsec + 1000000000 - _ts1.tv_nsec;
		}
		return 1;
	}

	if (_ts1.tv_nsec >= _ts2.tv_nsec) {
		ts->tv_sec = _ts1.tv_sec - _ts2.tv_sec;
		ts->tv_nsec = _ts1.tv_nsec - _ts2.tv_nsec;
	}
	else {
		ts->tv_sec = _ts1.tv_sec - 1 - _ts2.tv_sec;
		ts->tv_nsec = _ts1.tv_nsec + 1000000000 - _ts2.tv_nsec;
	}
	return -1;
}
