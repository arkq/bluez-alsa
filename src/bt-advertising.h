/*
 * BlueALSA - bt-advertising.h
 * SPDX-FileCopyrightText: 2025-2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BLUEALSA_BT_ADVERTISING_H_
#define BLUEALSA_BT_ADVERTISING_H_

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include <gio/gio.h>
#include <glib-object.h>
#include <glib.h>

#include "ba-adapter.h"
#include "error.h"

#define BLUETOOTH_TYPE_ADVERTISING (bluetooth_advertising_get_type())
G_DECLARE_FINAL_TYPE(BluetoothAdvertising, bluetooth_advertising,
		BLUETOOTH, ADVERTISING, GObject)
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(BluetoothAdvertising, g_object_unref)

BluetoothAdvertising * bluetooth_advertising_new(
		GDBusObjectManagerServer * manager,
		const char * path,
		const char * uuid,
		const char * name);

error_code_t bluetooth_advertising_set_service_data(
		BluetoothAdvertising * adv,
		const void * data,
		size_t len);

void bluetooth_advertising_register(
		BluetoothAdvertising * adv,
		struct ba_adapter * adapter,
		GAsyncReadyCallback callback,
		void * userdata);
bool bluetooth_advertising_register_finish(
		BluetoothAdvertising * adv,
		GAsyncResult * result,
		GError ** error);

void bluetooth_advertising_unregister_sync(
		BluetoothAdvertising * adv);

#endif
