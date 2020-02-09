/*
 * BlueALSA - bluez-iface.h
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_BLUEZIFACE_H_
#define BLUEALSA_BLUEZIFACE_H_

#include <gio/gio.h>

#define BLUEZ_SERVICE "org.bluez"

#define BLUEZ_IFACE_ADAPTER         BLUEZ_SERVICE ".Adapter1"
#define BLUEZ_IFACE_DEVICE          BLUEZ_SERVICE ".Device1"
#define BLUEZ_IFACE_MEDIA           BLUEZ_SERVICE ".Media1"
#define BLUEZ_IFACE_MEDIA_ENDPOINT  BLUEZ_SERVICE ".MediaEndpoint1"
#define BLUEZ_IFACE_MEDIA_TRANSPORT BLUEZ_SERVICE ".MediaTransport1"
#define BLUEZ_IFACE_PROFILE         BLUEZ_SERVICE ".Profile1"
#define BLUEZ_IFACE_PROFILE_MANAGER BLUEZ_SERVICE ".ProfileManager1"

#define BLUEZ_TRANSPORT_STATE_IDLE    "idle"
#define BLUEZ_TRANSPORT_STATE_PENDING "pending"
#define BLUEZ_TRANSPORT_STATE_ACTIVE  "active"

extern const GDBusInterfaceInfo bluez_iface_endpoint;
extern const GDBusInterfaceInfo bluez_iface_profile;

#endif
