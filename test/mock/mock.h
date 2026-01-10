/*
 * mock.h
 * SPDX-FileCopyrightText: 2016-2025 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <glib.h>

#include "service.h"

typedef struct BlueALSAMockService {
	MockService service;
	/* If non-zero, introduce fuzzing delay in various places. */
	unsigned int fuzzing_ms;
} BlueALSAMockService;

BlueALSAMockService * mock_bluealsa_service_new(char * name,
		BlueZMockService * bluez, OFonoMockService * ofono, UPowerMockService * upower);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(BlueALSAMockService, mock_service_free)

void mock_bluealsa_service_run(BlueALSAMockService * srv, GAsyncQueue * sync);
