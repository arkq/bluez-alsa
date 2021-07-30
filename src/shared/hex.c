/*
 * BlueALSA - hex.c
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "shared/hex.h"

#include <errno.h>
#include <stdio.h>

static const int hextable[255] = {
	['0'] = 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
	['A'] = 10, 11, 12, 13, 14, 15,
	['a'] = 10, 11, 12, 13, 14, 15,
};

/**
 * Encode a binary data into a hex string.
 *
 * @param bin A buffer with binary data.
 * @param hex A buffer where null-terminated hexadecimal string will be
 *   stored. This buffer has to be big enough to store n*2 + 1 bytes of
 *   data.
 * @param n The length of the binary buffer which shall be encoded.
 * @return This function returns length of the hex string. */
ssize_t bin2hex(const void *bin, char *hex, size_t n) {
	for (size_t i = 0; i < n; i++)
		sprintf(&hex[i * 2], "%.2x", ((unsigned char *)bin)[i]);
	return n * 2;
}

/**
 * Decode a hex string into a binary data.
 *
 * @param hex A buffer with hexadecimal string.
 * @param bin A buffer where decoded data will be stored. This buffer has to
 *   be big enough to store n/2 bytes of data.
 * @param n The length of the string which shall be decoded.
 * @return On success this function returns the size of the binary data. If
 *   an error has occurred, -1 is returned and errno is set to indicate the
 *   error. */
ssize_t hex2bin(const char *hex, void *bin, size_t n) {

	if (n % 2 != 0)
		return errno = EINVAL, -1;

	n /= 2;
	for (size_t i = 0; i < n; i++) {
		((char *)bin)[i] = hextable[(int)hex[i * 2]] << 4;
		((char *)bin)[i] |= hextable[(int)hex[i * 2 + 1]];
	}

	return n;
}
