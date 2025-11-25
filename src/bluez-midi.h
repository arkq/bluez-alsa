/*
 * BlueALSA - bluez-midi.h
 * SPDX-FileCopyrightText: 2023-2025 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BLUEALSA_BLUEZMIDI_H_
#define BLUEALSA_BLUEZMIDI_H_

#include <gio/gio.h>

#include "ba-adapter.h"

GDBusObjectManagerServer *bluez_midi_app_new(struct ba_adapter *adapter, const char *path);

#endif
