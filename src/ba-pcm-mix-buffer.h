/*
 * BlueALSA - ba-mix-buffer.h
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
 * Copyright (c) 2025 borine
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_BAMIXBUFFER_H_
#define BLUEALSA_BAMIXBUFFER_H_

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>

struct ba_mix_buffer {
	/* sample format */
	uint16_t format;
	uint8_t channels;
	/* physical bytes per frame */
	uint16_t frame_size;
	/* array storing the mixed frames */
	union {
		int16_t *s16;
		int32_t *s32;
		int64_t *s64;
		void *any;
	} data;
	/* Capacity of the buffer in samples */
	size_t size;
	/* The number of samples to be transferred  at one time */
	size_t period;
	/* Position of next read from the mix */
	_Atomic size_t mix_offset;
	/* Postion after last sample written to the mix */
	size_t end;
};

int ba_pcm_mix_buffer_init(struct ba_mix_buffer *buffer,
				uint16_t format, uint8_t channels,
				size_t buffer_frames, size_t period_frames);

void ba_pcm_mix_buffer_release(struct ba_mix_buffer *buffer);

bool ba_pcm_mix_buffer_at_threshold(struct ba_mix_buffer *buffer);

size_t ba_pcm_mix_buffer_calc_avail(const struct ba_mix_buffer *buffer, size_t start, size_t end);
bool ba_pcm_mix_buffer_empty(const struct ba_mix_buffer *buffer);
size_t ba_pcm_mix_buffer_delay(const struct ba_mix_buffer *buffer, size_t offset);

size_t ba_pcm_mix_buffer_add(struct ba_mix_buffer *buffer,
				intmax_t *offset, const void *data, size_t bytes);

size_t ba_pcm_mix_buffer_read(struct ba_mix_buffer *buffer,
				void *data, size_t frames, double *scale);

void ba_pcm_mix_buffer_clear(struct ba_mix_buffer *buffer);

#endif
