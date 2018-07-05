/*
 * BlueALSA - ffb.h
 * Copyright (c) 2016-2018 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_SHARED_FFB_H_
#define BLUEALSA_SHARED_FFB_H_

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/**
 * Convenience wrapper for FIFO-like buffer for uint8_t. */
typedef struct {
	/* pointer to the allocated memory block */
	uint8_t *data;
	/* pointer to the end of data */
	uint8_t *tail;
	/* size of the buffer */
	size_t size;
} ffb_uint8_t;

/**
 * Convenience wrapper for FIFO-like buffer for int16_t. */
typedef struct {
	int16_t *data;
	int16_t *tail;
	size_t size;
} ffb_int16_t;

/**
 * Allocate/reallocate resources for the FIFO-like buffer.
 *
 * @param p Pointer to the buffer structure.
 * @param s Number of the buffer unite blocks.
 * @return On success this function returns non-NULL value. */
#define ffb_init(p, s) \
	((p)->data = (p)->tail = realloc((p)->data, ((p)->size = s) * sizeof(*(p)->data)))

void ffb_uint8_free(ffb_uint8_t *ffb);
void ffb_int16_free(ffb_int16_t *ffb);

/**
 * Get number of unite blocks available for writing. */
#define ffb_len_in(p) ((p)->size - ffb_len_out(p))
/**
 * Get number of unite blocks available for reading. */
#define ffb_len_out(p) ((size_t)((p)->tail - (p)->data))

/**
 * Get number of bytes available for writing. */
#define ffb_blen_in(p) (ffb_len_in(p) * sizeof(*(p)->data))
/**
 * Get number of bytes available for reading. */
#define ffb_blen_out(p) (ffb_len_out(p) * sizeof(*(p)->data))

/**
 * Move the tail pointer by the given number of unite blocks. */
#define ffb_seek(p, s) ((p)->tail += s)

/**
 * Set the tail pointer to the beginning of the buffer. */
#define ffb_rewind(p) ((p)->tail = (p)->data)

/**
 * Shift data by the given number of unite blocks. */
#define ffb_shift(p, s) do { \
		memmove((p)->data, (p)->data + (s), sizeof(*(p)->data) * (ffb_len_out(p) - (s))); \
		(p)->tail -= s; \
	} while (0)

#endif
