/*
 * BlueALSA - codec-aptx.h
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_CODECAPTX_H_
#define BLUEALSA_CODECAPTX_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/**
 * Opaque apt-X encoder/decoder handle. */
typedef void * HANDLE_APTX;

#if ENABLE_APTX
HANDLE_APTX aptxenc_init(void);
# if HAVE_APTX_DECODE
HANDLE_APTX aptxdec_init(void);
# endif
#endif

#if ENABLE_APTX_HD
HANDLE_APTX aptxhdenc_init(void);
# if HAVE_APTX_HD_DECODE
HANDLE_APTX aptxhddec_init(void);
# endif
#endif

#if ENABLE_APTX
ssize_t aptxenc_encode(HANDLE_APTX handle, const int16_t *input, size_t samples,
		void *output, size_t *len);
# if HAVE_APTX_DECODE
ssize_t aptxdec_decode(HANDLE_APTX handle, const void *input, size_t len,
		int16_t *output, size_t *samples);
# endif
#endif

#if ENABLE_APTX_HD
ssize_t aptxhdenc_encode(HANDLE_APTX handle, const int32_t *input, size_t samples,
		void *output, size_t *len);
# if HAVE_APTX_HD_DECODE
ssize_t aptxhddec_decode(HANDLE_APTX handle, const void *input, size_t len,
		int32_t *output, size_t *samples);
# endif
#endif

#if ENABLE_APTX
void aptxenc_destroy(HANDLE_APTX handle);
# if HAVE_APTX_DECODE
void aptxdec_destroy(HANDLE_APTX handle);
# endif
#endif

#if ENABLE_APTX_HD
void aptxhdenc_destroy(HANDLE_APTX handle);
# if HAVE_APTX_HD_DECODE
void aptxhddec_destroy(HANDLE_APTX handle);
# endif
#endif


#endif
