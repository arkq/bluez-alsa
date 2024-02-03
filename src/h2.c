/*
 * BlueALSA - h2.c
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "h2.h"

#include <stdint.h>

/**
 * Find H2 synchronization header within given data.
 *
 * @param data Memory area to be scanned for the H2 synchronization header.
 * @param len Address from where the length of the data is read. Upon exit, the
 *   remaining length of the data will be stored in this variable (received
 *   length minus scanned length).
 * @return On success this function returns address of the first occurrence
 *   of the H2 synchronization header. Otherwise, it returns NULL. */
void *h2_header_find(const void *data, size_t *len) {

	const uint8_t *_data = data;
	size_t _len = *len;
	void *ptr = NULL;

	while (_len >= sizeof(h2_header_t)) {

		/* load 16-bit little-endian value from memory */
		const h2_header_t h2 = (_data[1] << 8) | _data[0];

		if (H2_GET_SYNCWORD(h2) == H2_SYNCWORD &&
				(H2_GET_SN0(h2) >> 1) == (H2_GET_SN0(h2) & 1) &&
				(H2_GET_SN1(h2) >> 1) == (H2_GET_SN1(h2) & 1)) {
			ptr = (void *)_data;
			goto final;
		}

		_data += 1;
		_len--;
	}

final:
	*len = _len;
	return ptr;
}
