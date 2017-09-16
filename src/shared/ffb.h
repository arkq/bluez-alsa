/*
 * BlueALSA - ffb.h
 * Copyright (c) 2016-2017 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_SHARED_FFB_H_
#define BLUEALSA_SHARED_FFB_H_

#include <stddef.h>
#include <string.h>

/**
 * Convenience wrapper for FIFO-like buffer. */
struct ffb {
	/* pointer to the allocated memory block */
	unsigned char *data;
	/* pointer to the end of data */
	unsigned char *tail;
	/* size of the buffer */
	size_t size;
};

int ffb_init(struct ffb *ffb, size_t size);
void ffb_free(struct ffb *ffb);

#define ffb_len_in(p) ((p)->size - ffb_len_out(p))
#define ffb_len_out(p) ((size_t)((p)->tail - (p)->data))

#define ffb_seek(p, len) ((p)->tail += len)
#define ffb_rewind(p, len) do { \
		memmove((p)->data, (p)->data + (len), ffb_len_out(p) - (len)); \
		(p)->tail -= len; \
	} while (0)

#endif
