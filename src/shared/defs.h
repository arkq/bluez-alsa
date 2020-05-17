/*
 * BlueALSA - defs.h
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_SHARED_DEFS_H_
#define BLUEALSA_SHARED_DEFS_H_

#include <string.h>

/**
 * Convenient macro for getting "on the stack" array size. */
#define ARRAYSIZE(a) (sizeof(a) / sizeof(*(a)))

/**
 * Cleanup callback casting wrapper for the brevity's sake. */
#define PTHREAD_CLEANUP(f) ((void (*)(void *))(void (*)(void))(f))

/**
 * Thread routing callback casting wrapper. */
#define PTHREAD_ROUTINE(f) ((void *(*)(void *))(f))

/**
 * Convert macro value to string. */
#define STRINGIZE(x) STRINGIZE_(x)
#define STRINGIZE_(x) #x

#endif
