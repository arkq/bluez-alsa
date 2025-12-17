/*
 * BlueALSA - bluez.h
 * SPDX-FileCopyrightText: 2016-2025 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BLUEALSA_BLUEZ_H_
#define BLUEALSA_BLUEZ_H_

#include <stdbool.h>

#include <glib.h>

#include "a2dp.h"
#include "ba-device.h"

#define BLUEZ_MEDIA_TRANSPORT_A2DP_VOLUME_MIN 0
#define BLUEZ_MEDIA_TRANSPORT_A2DP_VOLUME_MAX 127
#define BLUEZ_MEDIA_TRANSPORT_BAP_VOLUME_MIN 0
#define BLUEZ_MEDIA_TRANSPORT_BAP_VOLUME_MAX 255

enum bluez_media_transport_state {
	BLUEZ_MEDIA_TRANSPORT_STATE_IDLE,
	BLUEZ_MEDIA_TRANSPORT_STATE_PENDING,
	BLUEZ_MEDIA_TRANSPORT_STATE_BROADCASTING,
	BLUEZ_MEDIA_TRANSPORT_STATE_ACTIVE,
};

int bluez_init(void);
void bluez_destroy(void);

bool bluez_a2dp_set_configuration(
		const char *dbus_current_sep_path,
		const struct a2dp_sep_config *remote_sep_cfg,
		const void *configuration,
		GError **error);

void bluez_battery_provider_update(
		struct ba_device *device);

#endif
