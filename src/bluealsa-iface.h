/*
 * BlueALSA - bluealsa-iface.h
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
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
#define BLUEALSA_IFACE_RFCOMM  BLUEALSA_SERVICE ".RFCOMM1"

#define BLUEALSA_TRANSPORT_TYPE_A2DP_SINK   "A2DP-sink"
#define BLUEALSA_TRANSPORT_TYPE_A2DP_SOURCE "A2DP-source"
#define BLUEALSA_TRANSPORT_TYPE_HFP_AG      "HFP-AG"
#define BLUEALSA_TRANSPORT_TYPE_HFP_HF      "HFP-HF"
#define BLUEALSA_TRANSPORT_TYPE_HSP_AG      "HSP-AG"
#define BLUEALSA_TRANSPORT_TYPE_HSP_HS      "HSP-HS"

#define BLUEALSA_PCM_CTRL_DRAIN  "Drain"
#define BLUEALSA_PCM_CTRL_DROP   "Drop"
#define BLUEALSA_PCM_CTRL_PAUSE  "Pause"
#define BLUEALSA_PCM_CTRL_RESUME "Resume"

#define BLUEALSA_PCM_MODE_SINK   "sink"
#define BLUEALSA_PCM_MODE_SOURCE "source"

extern const GDBusInterfaceInfo bluealsa_iface_manager;
extern const GDBusInterfaceInfo bluealsa_iface_pcm;
extern const GDBusInterfaceInfo bluealsa_iface_rfcomm;

#endif
