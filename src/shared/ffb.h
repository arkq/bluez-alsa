/*
 * BlueALSA - ffb.h
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
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

/**
 * Convenience wrapper for FIFO-like buffer. */
typedef struct {
	/* pointer to the allocated memory block */
	void *data;
	/* pointer to the end of data */
	void *tail;
	/* number of elements in the buffer */
	size_t nmemb;
	/* the size of each element */
	size_t size;
} ffb_t;

int ffb_init(ffb_t *ffb, size_t nmemb, size_t size);
void ffb_free(ffb_t *ffb);

#define ffb_init_uint8_t(p, n) ffb_init(p, n, sizeof(uint8_t))
#define ffb_init_int16_t(p, n) ffb_init(p, n, sizeof(int16_t))
#define ffb_init_int32_t(p, n) ffb_init(p, n, sizeof(int32_t))

/**
 * Get number of unite blocks available for writing. */
#define ffb_len_in(p) (ffb_blen_in(p) / (p)->size)
/**
 * Get number of unite blocks available for reading. */
#define ffb_len_out(p) (ffb_blen_out(p) / (p)->size)

/**
 * Get number of bytes available for writing. */
#define ffb_blen_in(p) ((p)->nmemb * (p)->size - ffb_blen_out(p))
/**
 * Get number of bytes available for reading. */
#define ffb_blen_out(p) ((size_t)((uint8_t *)(p)->tail - (uint8_t *)(p)->data))

/**
 * Move the tail pointer by the given number of unite blocks. */
#define ffb_seek(p, n) ((p)->tail = (uint8_t *)(p)->tail + (n) * (p)->size)

/**
 * Set the tail pointer to the beginning of the buffer. */
#define ffb_rewind(p) ((p)->tail = (p)->data)

/**
 * Shift data by the given number of elements. */
int ffb_shift(ffb_t *ffb, size_t nmemb);

#endif
