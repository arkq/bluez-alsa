/*
 * BlueALSA - ffb.c
 * Copyright (c) 2016-2018 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "shared/ffb.h"


/**
 * Free resources allocated by the ffb_uint8_init().
 *
 * @param ffb Pointer to initialized buffer structure. */
void ffb_uint8_free(ffb_uint8_t *ffb) {
	if (ffb->data == NULL)
		return;
	free(ffb->data);
	ffb->data = NULL;
}

/**
 * Free resources allocated by the ffb_int16_init().
 *
 * @param ffb Pointer to initialized buffer structure. */
void ffb_int16_free(ffb_int16_t *ffb) {
	if (ffb->data == NULL)
		return;
	free(ffb->data);
	ffb->data = NULL;
}
