/*
 * BlueALSA - bluez-midi.h
 * Copyright (c) 2016-2023 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_BLUEZMIDI_H_
#define BLUEALSA_BLUEZMIDI_H_

#include <gio/gio.h>

#include "ba-adapter.h"

GDBusObjectManagerServer *bluez_midi_app_new(struct ba_adapter *adapter, const char *path);

#endif
