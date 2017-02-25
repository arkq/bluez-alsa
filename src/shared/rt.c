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
