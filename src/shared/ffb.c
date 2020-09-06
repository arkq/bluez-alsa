/*
 * BlueALSA - ffb.c
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "shared/ffb.h"

#include <stdlib.h>
#include <string.h>

/**
 * Allocate/reallocate resources for the FIFO-like buffer.
 *
 * @param ffb Pointer to the buffer structure.
 * @param nmemb Number of elements in the buffer.
 * @param size The size of the element.
 * @return On success this function returns 0, otherwise -1. */
int ffb_init(ffb_t *ffb, size_t nmemb, size_t size) {

	void *ptr;
	if ((ptr = realloc(ffb->data, nmemb * size)) == NULL)
		return -1;

	ffb->data = ffb->tail = ptr;
	ffb->nmemb = nmemb;
	ffb->size = size;

	return 0;
}

/**
 * Free resources allocated with the ffb_init().
 *
 * @param ffb Pointer to initialized buffer structure. */
void ffb_free(ffb_t *ffb) {
	if (ffb->data == NULL)
		return;
	free(ffb->data);
	ffb->data = NULL;
}

/**
 * Shift data by the given number of elements.
 *
 * @param ffb Pointer to initialized buffer structure.
 * @param nmemb Number of elements to shift.
 * @return Number of shifted elements. Might be less than requested
 *   nmemb in case where ffb_len_out(ffb) < nmemb. */
int ffb_shift(ffb_t *ffb, size_t nmemb) {

	const size_t blen_out = ffb_blen_out(ffb);
	size_t blen_shift = nmemb * ffb->size;

	if (blen_shift > blen_out)
		blen_shift = blen_out;

	const size_t blen_move = blen_out - blen_shift;
	memmove(ffb->data, (uint8_t *)ffb->data + blen_shift, blen_move);
	ffb->tail = (uint8_t *)ffb->tail - blen_shift;

	return blen_shift / ffb->size;
}
