/*
 * BlueALSA - ofono-iface.h
 * Copyright (c) 2018 Thierry Bultel (thierry.bultel@iot.bzh)
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_OFONO_IFACE_H_
#define BLUEALSA_OFONO_IFACE_H_

#include <gio/gio.h>

#define OFONO_SERVICE "org.ofono"
#define HF_AUDIO_AGENT_INTERFACE OFONO_SERVICE ".HandsfreeAudioAgent"
#define HF_AUDIO_MANAGER_INTERFACE OFONO_SERVICE ".HandsfreeAudioManager"
#define HF_AUDIO_CARD_INTERFACE OFONO_SERVICE ".HandsfreeAudioCard"

extern GDBusInterfaceInfo ofono_iface_profile;

#endif
