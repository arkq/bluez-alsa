/*
 * BlueALSA - bluealsa-iface.h
 * Copyright (c) 2016-2019 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_BLUEALSAIFACE_H_
#define BLUEALSA_BLUEALSAIFACE_H_

#include <gio/gio.h>

#define BLUEALSA_SERVICE "org.bluealsa"

#define BLUEALSA_IFACE_MANAGER BLUEALSA_SERVICE ".Manager1"
#define BLUEALSA_IFACE_PCM     BLUEALSA_SERVICE ".PCM1"

#define BLUEALSA_PCM_CTRL_DRAIN  "Drain"
#define BLUEALSA_PCM_CTRL_DROP   "Drop"
#define BLUEALSA_PCM_CTRL_PAUSE  "Pause"
#define BLUEALSA_PCM_CTRL_RESUME "Resume"

#define BLUEALSA_PCM_MODE_SINK   "sink"
#define BLUEALSA_PCM_MODE_SOURCE "source"

const GDBusInterfaceInfo bluealsa_iface_manager;
const GDBusInterfaceInfo bluealsa_iface_pcm;

#endif
