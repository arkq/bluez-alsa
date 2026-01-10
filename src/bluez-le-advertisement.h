/*
 * BlueALSA - bluez-le-advertisement.h
 * SPDX-FileCopyrightText: 2025 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BLUEALSA_BLUEZLEADVERTISEMENT_H_
#define BLUEALSA_BLUEZLEADVERTISEMENT_H_

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include <gio/gio.h>
#include <glib-object.h>
#include <glib.h>

#include "ba-adapter.h"
#include "error.h"

#define BLUEZ_TYPE_LE_ADVERTISEMENT (bluez_le_advertisement_get_type())
G_DECLARE_FINAL_TYPE(BlueZLEAdvertisement, bluez_le_advertisement, BLUEZ, LE_ADVERTISEMENT, GObject)
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(BlueZLEAdvertisement, g_object_unref)

BlueZLEAdvertisement * bluez_le_advertisement_new(
		GDBusObjectManagerServer * manager,
		const char * uuid,
		const char * name,
		const char * path);

error_code_t bluez_le_advertisement_set_service_data(
		BlueZLEAdvertisement * adv,
		const uint8_t * data,
		size_t len);

void bluez_le_advertisement_register(
		BlueZLEAdvertisement * adv,
		struct ba_adapter * adapter,
		GAsyncReadyCallback callback,
		void * userdata);
bool bluez_le_advertisement_register_finish(
		BlueZLEAdvertisement * adv,
		GAsyncResult * result,
		GError ** error);

void bluez_le_advertisement_unregister_sync(
		BlueZLEAdvertisement * adv);

#endif
