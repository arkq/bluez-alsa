/*
 * BlueALSA - bt-asha.h
 * SPDX-FileCopyrightText: 2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BLUEALSA_BT_ASHA_H_
#define BLUEALSA_BT_ASHA_H_

#include <glib-object.h>
#include <glib.h>

#include "ba-adapter.h"

#define BLUETOOTH_TYPE_ASHA (bluetooth_asha_get_type())
G_DECLARE_FINAL_TYPE(BluetoothASHA, bluetooth_asha, BLUETOOTH, ASHA, GObject)
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(BluetoothASHA, g_object_unref)

BluetoothASHA * bluetooth_asha_new(
		struct ba_adapter * adapter,
		const char * path);

#endif
