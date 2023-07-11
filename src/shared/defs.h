/*
 * BlueALSA - defs.h
 * Copyright (c) 2016-2023 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_SHARED_DEFS_H_
#define BLUEALSA_SHARED_DEFS_H_

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

#endif
