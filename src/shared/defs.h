/*
 * BlueALSA - defs.h
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_SHARED_DEFS_H_
#define BLUEALSA_SHARED_DEFS_H_

#include <endian.h>
#include <string.h>

/**
 * Convenient macro for getting "on the stack" array size. */
#define ARRAYSIZE(a) (sizeof(a) / sizeof(*(a)))

/**
 * Divide integers with rounding. */
#define DIV_ROUND(n, d) (((n) + (d) / 2) / (d))

/**
 * Divide integers with rounding up. */
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))

/**
 * Convert any pointer to a mutable pointer. */
#define MUTABLE(v) ((void *)(v))

/**
 * Cleanup callback casting wrapper for the brevity's sake. */
#define PTHREAD_CLEANUP(f) ((void (*)(void *))(void (*)(void))(f))

/**
 * Thread function callback casting wrapper. */
#define PTHREAD_FUNC(f) ((void *(*)(void *))(f))

/**
 * Qsort comparision function casting wrapper. */
#define QSORT_COMPAR(f) ((int (*)(const void *, const void *))(f))

/**
 * Convert macro value to string. */
#define STRINGIZE(x) STRINGIZE_(x)
#define STRINGIZE_(x) #x

/**
 * Swap endianness of 16-bit value in compile-time. */
#define SWAP_UINT16(x) ((((x) << 8) & 0xFF00) | (x) >> 8)

/**
 * Swap endianness of 32-bit value in compile-time. */
#define SWAP_UINT32(x) ( \
		((x) << 24) | (((x) << 8) & 0x00FF0000) | \
		(((x) >> 8) & 0x0000FF00) | ((x) >> 24))

#if __BYTE_ORDER == __LITTLE_ENDIAN
# define HTOBE16(x) SWAP_UINT16(x)
# define HTOBE32(x) SWAP_UINT32(x)
# define HTOLE16(x) (x)
# define HTOLE32(x) (x)
#elif __BYTE_ORDER == __BIG_ENDIAN
# define HTOBE16(x) (x)
# define HTOBE32(x) (x)
# define HTOLE16(x) SWAP_UINT16(x)
# define HTOLE32(x) SWAP_UINT32(x)
#else
# error "Unknown byte order!"
#endif

#endif
