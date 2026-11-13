/*
 * BlueALSA - media-control.h
 * SPDX-FileCopyrightText: 2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BLUEALSA_APLAY_MEDIACONTROL_H_
#define BLUEALSA_APLAY_MEDIACONTROL_H_

#include <dbus/dbus.h>
#include <stdbool.h>

#include "shared/dbus-client-pcm.h"

typedef enum {
	MEDIA_CONTROL_STATE_UNKNOWN,
	MEDIA_CONTROL_STATE_PAUSED,
	MEDIA_CONTROL_STATE_PLAYING,
} media_control_state;

struct media_control {
	/* BlueZ D-Bus player path */
	char player_path[128];
	/* true if player_path is valid */
	bool connected;
	media_control_state state;
};


inline static bool media_control_pcm_is_media(const struct ba_pcm * ba_pcm) {
	return ba_pcm->transport & BA_PCM_TRANSPORT_MASK_A2DP;
}

bool media_control_init(struct media_control * mc, const char * device_path,
		DBusConnection * conn);
bool media_control_pause_player(const struct media_control * mc, DBusConnection * conn);

dbus_bool_t media_control_dbus_add_signal_handler(struct ba_dbus_ctx * dbus_ctx);

#endif
