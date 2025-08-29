/*
 * BlueALSA - hex.c
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "hex.h"

#include <errno.h>

/**
 * Encode a binary data into a hex string.
 *
 * @param bin A buffer with binary data.
 * @param hex A buffer where null-terminated hexadecimal string will be
 *   stored. This buffer has to be big enough to store n*2 + 1 bytes of
 *   data.
 * @param n The length of the binary buffer which shall be encoded.
 * @return This function returns length of the hex string. */
ssize_t bin2hex(const void * restrict bin, char * restrict hex, size_t n) {

	static const char map_bin2hex[] = "0123456789abcdef";

	for (size_t i = 0; i < n; i++) {
		const unsigned char *src = bin;
		hex[i * 2] = map_bin2hex[src[i] >> 4];
		hex[i * 2 + 1] = map_bin2hex[src[i] & 0x0f];
	}

	hex[n * 2] = '\0';
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
ssize_t hex2bin(const char * restrict hex, void * restrict bin, size_t n) {

	static const int map_hex2bin[256] = {
		['0'] = 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
		['A'] = 10, 11, 12, 13, 14, 15,
		['a'] = 10, 11, 12, 13, 14, 15,
	};

	if (n % 2 != 0)
		return errno = EINVAL, -1;

	n /= 2;
	unsigned char *out = bin;
	for (size_t i = 0; i < n; i++) {
		const unsigned char c1 = hex[i * 2];
		const unsigned char c2 = hex[i * 2 + 1];
		out[i] = (map_hex2bin[c1] << 4) | map_hex2bin[c2];
	}

	return n;
}
