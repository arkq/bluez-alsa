/*
 * BlueALSA - rc.h
 * SPDX-FileCopyrightText: 2025 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BLUEALSA_SHARED_RC_H_
#define BLUEALSA_SHARED_RC_H_

#include <stdatomic.h>

/**
 * Reference counter free callback. */
typedef void (*rc_free_t)(void * ptr);

/**
 * Reference counter header.
 *
 * This structure must be placed at the beginning of the reference counted
 * object and initialized using rc_init() function. Then, the rc_ref() and
 * rc_unref() functions may be used to manage the reference counter. */
typedef struct rc {
	atomic_int count;
	rc_free_t callback;
} rc_t;

/**
 * Initialize the reference counter. */
static inline void rc_init(rc_t * rc, rc_free_t callback) {
	atomic_init(&rc->count, 1);
	rc->callback = callback;
}

/**
 * Increase the reference counter. */
static inline void * rc_ref(void * ptr) {
	atomic_fetch_add_explicit(&((rc_t *)ptr)->count, 1, memory_order_relaxed);
	return ptr;
}

int rc_unref_with_count(void * ptr);

/**
 * Decrease the reference counter. */
static inline void rc_unref(void * ptr) {
	rc_unref_with_count(ptr);
}

#endif
