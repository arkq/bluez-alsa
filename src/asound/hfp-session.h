/*
 * hfp-session.h
 * Copyright (c) 2016-2022 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef HFP_SESSION_H_
#define HFP_SESSION_H_

#include <bluetooth/bluetooth.h>
#include <dbus/dbus.h>
#include <limits.h>

#include "shared/dbus-client.h"

struct hfp_session {
	char rfcomm_path[128];
	char lock_file[PATH_MAX + 1];
	int lock_fd;
};

int hfp_session_init(struct hfp_session **phfp, const char *device_path, const bdaddr_t *addr);
int hfp_session_begin(struct hfp_session *hfp, struct ba_dbus_ctx *dbus_ctx);
int hfp_session_end(struct hfp_session *hfp, struct ba_dbus_ctx *dbus_ctx);
void hfp_session_free(struct hfp_session *hfp);

#endif
