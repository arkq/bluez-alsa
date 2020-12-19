/*
 * BlueALSA - bluealsa-mix-buffer.h
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */
#ifndef BLUEALSA_MIX_BUFFER_H
#define BLUEALSA_MIX_BUFFER_H

#include <stdint.h>
#include <stdbool.h>

struct bluealsa_mix_buffer {
	/* sample format */
	uint16_t format;
	uint8_t channels;
	/* physical bytes per frame */
	uint16_t frame_size;
	union {
		int16_t *s16;
		int32_t *s32;
		int64_t *s64;
		void *any;
	} data;
	/* Capacity of the buffer in samples */
	size_t size;
	/* The number of frames to be transferred  at one time */
	size_t period;
	/* Position of next read from the mix */
	size_t mix_offset;
	/* Postion after last sample written to the mix */
	size_t end;
};

int bluealsa_mix_buffer_init(struct bluealsa_mix_buffer *buffer,
                             uint16_t format, uint8_t channels,
                             size_t buffer_frames, size_t period_frames);

void bluealsa_mix_buffer_release(struct bluealsa_mix_buffer *buffer);

size_t bluealsa_mix_buffer_avail(struct bluealsa_mix_buffer *buffer);
bool bluealsa_mix_buffer_empty(struct bluealsa_mix_buffer *buffer);
size_t bluealsa_mix_buffer_delay(struct bluealsa_mix_buffer *buffer,
                                                                size_t offset);

void bluealsa_mix_buffer_reset(struct bluealsa_mix_buffer *buffer);

ssize_t bluealsa_mix_buffer_add(struct bluealsa_mix_buffer *buffer,
                                    ssize_t *offset, void *data, size_t bytes);

size_t bluealsa_mix_buffer_read(struct bluealsa_mix_buffer *buffer,
                                     void *data, size_t frames, double *scale);

#endif /* BLUEALSA_MIX_BUFFER_H */
