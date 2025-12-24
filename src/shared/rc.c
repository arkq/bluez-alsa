/*
 * BlueALSA - rc.h
 * SPDX-FileCopyrightText: 2025 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#include "rc.h"

#include <stdatomic.h>

/**
 * Decrease the reference counter.
 *
 * @param ptr Pointer to the reference counted object.
 * @return The updated reference count. */
int rc_unref_with_count(void * ptr) {
	int c;
	rc_t * rc = ptr;
	if ((c = atomic_fetch_sub_explicit(&rc->count, 1, memory_order_acq_rel)) == 1)
		rc->callback(ptr);
	return c - 1;
}
