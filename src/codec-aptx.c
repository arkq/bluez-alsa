/*
 * BlueALSA - codec-aptx.c
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "codec-aptx.h"

#include <endian.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <openaptx.h>

#include "shared/defs.h"
#include "shared/log.h"

#if ENABLE_APTX
/**
 * Initialize apt-X encoder handler.
 *
 * @returns On success, this function returns initialized apt-X encoder
 *   handler. On error, NULL is returned. */
HANDLE_APTX aptxenc_init(void) {
#if WITH_LIBOPENAPTX
	return aptx_init(0);
#else
	APTXENC handle;
	if ((handle = malloc(SizeofAptxbtenc())) == NULL ||
			aptxbtenc_init(handle, __BYTE_ORDER == __LITTLE_ENDIAN) != 0) {
		free(handle);
		return NULL;
	}
	return handle;
#endif
}
#endif

#if ENABLE_APTX && HAVE_APTX_DECODE
/**
 * Initialize apt-X decoder handler.
 *
 * @returns On success, this function returns initialized apt-X decoder
 *   handler. On error, NULL is returned. */
HANDLE_APTX aptxdec_init(void) {
#if WITH_LIBOPENAPTX
	return aptx_init(0);
#else
	APTXDEC handle;
	if ((handle = malloc(SizeofAptxbtdec())) == NULL ||
			aptxbtdec_init(handle, __BYTE_ORDER == __LITTLE_ENDIAN) != 0) {
		free(handle);
		return NULL;
	}
	return handle;
#endif
}
#endif

#if ENABLE_APTX_HD
/**
 * Initialize apt-X HD encoder handler.
 *
 * @returns On success, this function returns initialized apt-X encoder
 *   handler. On error, NULL is returned. */
HANDLE_APTX aptxhdenc_init(void) {
#if WITH_LIBOPENAPTX
	return aptx_init(1);
#else
	APTXENC handle;
	if ((handle = malloc(SizeofAptxhdbtenc())) == NULL ||
			aptxhdbtenc_init(handle, false) != 0) {
		free(handle);
		return NULL;
	}
	return handle;
#endif
}
#endif

#if ENABLE_APTX_HD && HAVE_APTX_HD_DECODE
/**
 * Initialize apt-X HD decoder handler.
 *
 * @returns On success, this function returns initialized apt-X decoder
 *   handler. On error, NULL is returned. */
HANDLE_APTX aptxhddec_init(void) {
#if WITH_LIBOPENAPTX
	return aptx_init(1);
#else
	APTXDEC handle;
	if ((handle = malloc(SizeofAptxhdbtdec())) == NULL ||
			aptxhdbtdec_init(handle, false) != 0) {
		free(handle);
		return NULL;
	}
	return handle;
#endif
}
#endif

#if ENABLE_APTX
/**
 * Encode stereo PCM.
 *
 * @returns On success, this function returns the number of processed input
 *   samples. On error, -1 is returned. */
ssize_t aptxenc_encode(HANDLE_APTX handle, const int16_t *input, size_t samples,
		void *output, size_t *len) {

	if (samples < 8 || *len < 4)
		return errno = EINVAL, -1;

#if WITH_LIBOPENAPTX

	const uint8_t pcm[3 /* 24bit */ * 8 /* 4 samples * 2 channels */] = {
		0, input[0], input[0] >> 8, 0, input[1], input[1] >> 8,
		0, input[2], input[2] >> 8, 0, input[3], input[3] >> 8,
		0, input[4], input[4] >> 8, 0, input[5], input[5] >> 8,
		0, input[6], input[6] >> 8, 0, input[7], input[7] >> 8,
	};

	size_t rv;
	if ((rv = aptx_encode(handle, pcm, sizeof(pcm), output, *len, len)) != sizeof(pcm))
		return -1;

	return rv / 3;

#else

	int32_t pcm_l[4] = { input[0], input[2], input[4], input[6] };
	int32_t pcm_r[4] = { input[1], input[3], input[5], input[7] };

	if (aptxbtenc_encodestereo(handle, pcm_l, pcm_r, output) != 0)
		return -1;

	*len = 4;
	return 8;

#endif
}
#endif

#if ENABLE_APTX && HAVE_APTX_DECODE
/**
 * Decode stereo PCM.
 *
 * @returns On success, this function returns the number of processed input
 *   bytes. On error, -1 is returned. */
ssize_t aptxdec_decode(HANDLE_APTX handle, const void *input, size_t len,
		int16_t *output, size_t *samples) {

	if (len < 4 || *samples < 8)
		return errno = EINVAL, -1;

#if WITH_LIBOPENAPTX

	uint8_t pcm[3 /* 24bit */ * 8 /* 4 samples * 2 channels */ * 2];
	size_t written, dropped;
	int synced;

	if ((len = aptx_decode_sync(handle, input, 4, pcm, sizeof(pcm),
					&written, &synced, &dropped)) != 4)
		return -1;

	if (!synced && dropped > 0)
		info("Apt-X stream out of sync: Dropped bytes: %zd", dropped);

	size_t i;
	for (i = 0; i < written / 3 / 2; i++) {
		*output++ = pcm[i * 6 + 0 + 1] | (pcm[i * 6 + 0 + 2] << 8);
		*output++ = pcm[i * 6 + 3 + 1] | (pcm[i * 6 + 3 + 2] << 8);
	}

	*samples = written / 3;
	return len;

#else

	int32_t pcm_l[4], pcm_r[4];
	if (aptxbtdec_decodestereo(handle, pcm_l, pcm_r, input) != 0)
		return -1;

	size_t i;
	for (i = 0; i < ARRAYSIZE(pcm_l); i++) {
		*output++ = pcm_l[i];
		*output++ = pcm_r[i];
	}

	*samples = 8;
	return 4;

#endif
}
#endif

#if ENABLE_APTX_HD
/**
 * Encode stereo PCM (HD variant).
 *
 * @returns On success, this function returns the number of processed input
 *   samples. On error, -1 is returned. */
ssize_t aptxhdenc_encode(HANDLE_APTX handle, const int32_t *input, size_t samples,
		void *output, size_t *len) {

	if (samples < 8 || *len < 6)
		return errno = EINVAL, -1;

#if WITH_LIBOPENAPTX

	const uint8_t pcm[3 /* 24bit */ * 8 /* 4 samples * 2 channels */] = {
		input[0], input[0] >> 8, input[0] >> 16, input[1], input[1] >> 8, input[1] >> 16,
		input[2], input[2] >> 8, input[2] >> 16, input[3], input[3] >> 8, input[3] >> 16,
		input[4], input[4] >> 8, input[4] >> 16, input[5], input[5] >> 8, input[5] >> 16,
		input[6], input[6] >> 8, input[6] >> 16, input[7], input[7] >> 8, input[7] >> 16,
	};

	size_t rv;
	if ((rv = aptx_encode(handle, pcm, sizeof(pcm), output, *len, len)) != sizeof(pcm))
		return -1;

	return rv / 3;

#else

	int32_t pcm_l[4] = { input[0], input[2], input[4], input[6] };
	int32_t pcm_r[4] = { input[1], input[3], input[5], input[7] };
	uint32_t code[2];

	if (aptxhdbtenc_encodestereo(handle, pcm_l, pcm_r, code) != 0)
		return -1;

	((uint8_t *)output)[0] = code[0] >> 16;
	((uint8_t *)output)[1] = code[0] >> 8;
	((uint8_t *)output)[2] = code[0];
	((uint8_t *)output)[3] = code[1] >> 16;
	((uint8_t *)output)[4] = code[1] >> 8;
	((uint8_t *)output)[5] = code[1];

	*len = 6;
	return 8;

#endif
}
#endif

#if ENABLE_APTX_HD && HAVE_APTX_HD_DECODE
/**
 * Decode stereo PCM (HD variant).
 *
 * @returns On success, this function returns the number of processed input
 *   bytes. On error, -1 is returned. */
ssize_t aptxhddec_decode(HANDLE_APTX handle, const void *input, size_t len,
		int32_t *output, size_t *samples) {

	if (len < 6 || *samples < 8)
		return errno = EINVAL, -1;

#if WITH_LIBOPENAPTX

	uint8_t pcm[3 /* 24bit */ * 8 /* 4 samples * 2 channels */ * 2];
	size_t written, dropped;
	int synced;

	if ((len = aptx_decode_sync(handle, input, 6, pcm, sizeof(pcm),
					&written, &synced, &dropped)) != 6)
		return -1;

	if (!synced && dropped > 0)
		info("Apt-X HD stream out of sync: Dropped bytes: %zd", dropped);

	size_t i;
	int32_t base;
	for (i = 0; i < written / 3 / 2; i++) {
		base = pcm[i * 6 + 0 + 2] & 0x80 ? 0xFF000000 : 0;
		*output++ = base | pcm[i * 6 + 0 + 0] | (pcm[i * 6 + 0 + 1] << 8) | (pcm[i * 6 + 0 + 2] << 16);
		base = pcm[i * 6 + 3 + 2] & 0x80 ? 0xFF000000 : 0;
		*output++ = base | pcm[i * 6 + 3 + 0] | (pcm[i * 6 + 3 + 1] << 8) | (pcm[i * 6 + 3 + 2] << 16);
	}

	*samples = written / 3;
	return len;

#else

	const uint32_t code[2] = {
		(((uint8_t *)input)[0] << 16) | (((uint8_t *)input)[1] << 8) | ((uint8_t *)input)[2],
		(((uint8_t *)input)[3] << 16) | (((uint8_t *)input)[4] << 8) | ((uint8_t *)input)[5] };
	int32_t pcm_l[4], pcm_r[4];

	if (aptxhdbtdec_decodestereo(handle, pcm_l, pcm_r, code) != 0)
		return -1;

	size_t i;
	for (i = 0; i < ARRAYSIZE(pcm_l); i++) {
		*output++ = pcm_l[i];
		*output++ = pcm_r[i];
	}

	*samples = 8;
	return 6;

#endif
}
#endif

#if ENABLE_APTX
/**
 * Destroy apt-X encoder and free handler.
 *
 * @param handle Initialized encoder handler. */
void aptxenc_destroy(HANDLE_APTX handle) {
#if WITH_LIBOPENAPTX
	aptx_finish(handle);
#else
	if (aptxbtenc_destroy != NULL)
		aptxbtenc_destroy(handle);
	free(handle);
#endif
}
#endif

#if ENABLE_APTX && HAVE_APTX_DECODE
/**
 * Destroy apt-X decoder and free handler.
 *
 * @param handle Initialized decoder handler. */
void aptxdec_destroy(HANDLE_APTX handle) {
#if WITH_LIBOPENAPTX
	aptx_finish(handle);
#else
	aptxbtdec_destroy(handle);
	free(handle);
#endif
}
#endif

#if ENABLE_APTX_HD
/**
 * Destroy apt-X HD encoder and free handler.
 *
 * @param handle Initialized encoder handler. */
void aptxhdenc_destroy(HANDLE_APTX handle) {
#if WITH_LIBOPENAPTX
	aptx_finish(handle);
#else
	if (aptxhdbtenc_destroy != NULL)
		aptxhdbtenc_destroy(handle);
	free(handle);
#endif
}
#endif

#if ENABLE_APTX_HD && HAVE_APTX_HD_DECODE
/**
 * Destroy apt-X HD decoder and free handler.
 *
 * @param handle Initialized decoder handler. */
void aptxhddec_destroy(HANDLE_APTX handle) {
#if WITH_LIBOPENAPTX
	aptx_finish(handle);
#else
	aptxhdbtdec_destroy(handle);
	free(handle);
#endif
}
#endif
