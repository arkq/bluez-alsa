/*
 * sine.inc
 * vim: ft=c
 *
 * Copyright (c) 2016-2017 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <limits.h>
#include <math.h>
#include <stdlib.h>

/**
 * Generate sine PCM signal.
 *
 * @param buffer Address to the buffer, where the data will be stored.
 * @param size The size of the buffer. This value shall be a multiplication
 *   of the number of channels.
 * @param channels Number of PCM channels.
 * @param x Sampling argument of the sine function.
 * @param f Required sine frequency divided by the PCM sampling frequency.
 * @return Updated x parameter. One may use this value for a next call, in
 *   order to generate smooth sine curve. */
int snd_pcm_sine_s16le(int16_t *buffer, size_t size, int channels, int x, float f) {
	size_t i;
	for (i = 0; i < size; x++, i += channels) {
		buffer[i] = sin(2 * M_PI * f * x) * SHRT_MAX;
		if (channels == 2)
			buffer[i + 1] = buffer[i];
	}
	return x;
}
