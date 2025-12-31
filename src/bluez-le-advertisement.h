/*
 * BlueALSA - bluez-le-advertisement.h
 * SPDX-FileCopyrightText: 2025 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BLUEALSA_BLUEZLEADVERTISEMENT_H_
#define BLUEALSA_BLUEZLEADVERTISEMENT_H_

#include <gio/gio.h>

#include "ba-adapter.h"

struct bluez_le_advertisement;

struct bluez_le_advertisement * bluez_le_advertisement_new(
		GDBusObjectManagerServer * manager,
		const char * uuid,
		const char * name,
		const char * path);

void bluez_le_advertisement_register(
		struct bluez_le_advertisement * adv,
		struct ba_adapter * adapter);
void bluez_le_advertisement_unregister_sync(
		struct bluez_le_advertisement * adv);

#endif
