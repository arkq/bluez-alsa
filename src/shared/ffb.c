/*
 * BlueALSA - ffb.c
 * Copyright (c) 2016-2017 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "shared/ffb.h"

#include <stdlib.h>


/**
 * Allocated resources for FIFO-like buffer.
 *
 * @param ffb Pointer to the buffer structure which should be initialized.
 * @param size Size of the buffer in bytes.
 * @return On success this function returns 0. Otherwise, -1 is returned and
 *	 errno is set to indicate the error. */
int ffb_init(struct ffb *ffb, size_t size) {
	if ((ffb->data = ffb->tail = malloc(size)) == NULL)
		return -1;
	ffb->size = size;
	return 0;
}

/**
 * Free resources allocated by the ffb_init().
 *
 * @param ffb Pointer to initialized buffer structure. */
void ffb_free(struct ffb *ffb) {
	if (ffb->data == NULL)
		return;
	free(ffb->data);
	ffb->data = NULL;
	ffb->tail = NULL;
	ffb->size = 0;
}
