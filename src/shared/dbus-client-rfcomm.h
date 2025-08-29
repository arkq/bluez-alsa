/*
 * BlueALSA - dbus-rfcomm.h
 * Copyright (c) 2016-2023 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_SHARED_DBUSCLIENTRFCOMM_H_
#define BLUEALSA_SHARED_DBUSCLIENTRFCOMM_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stddef.h>

#include <dbus/dbus.h>

#include "dbus-client.h"

/**
 * BlueALSA RFCOMM property object. */
struct ba_rfcomm_props {
	/* BlueALSA transport type */
	char transport[7];
	/* remote device supported features */
	char **features;
	size_t features_len;
	/* remote device battery level */
	int battery;
};

dbus_bool_t ba_dbus_rfcomm_props_get(
		struct ba_dbus_ctx *ctx,
		const char *rfcomm_path,
		struct ba_rfcomm_props *props,
		DBusError *error);

void ba_dbus_rfcomm_props_free(
		struct ba_rfcomm_props *props);

dbus_bool_t ba_dbus_rfcomm_open(
		struct ba_dbus_ctx *ctx,
		const char *rfcomm_path,
		int *fd_rfcomm,
		DBusError *error);

#endif
