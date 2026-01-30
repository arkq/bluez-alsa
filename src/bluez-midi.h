/*
 * BlueALSA - bluez-midi.h
 * SPDX-FileCopyrightText: 2023-2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BLUEALSA_BLUEZ_MIDI_H_
#define BLUEALSA_BLUEZ_MIDI_H_

#include <glib-object.h>
#include <glib.h>

#include "ba-adapter.h"

#define BLUETOOTH_TYPE_MIDI (bluetooth_midi_get_type())
G_DECLARE_FINAL_TYPE(BluetoothMIDI, bluetooth_midi, BLUETOOTH, MIDI, GObject)
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(BluetoothMIDI, g_object_unref)

BluetoothMIDI * bluetooth_midi_new(
		struct ba_adapter * adapter,
		const char * path);

#endif
