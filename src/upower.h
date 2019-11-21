/*
 * BlueALSA - upower.h
 * Copyright (c) 2016-2019 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_UPOWER_H_
#define BLUEALSA_UPOWER_H_

#define UPOWER_SERVICE "org.freedesktop.UPower"

#define UPOWER_IFACE_UPOWER           UPOWER_SERVICE
#define UPOWER_IFACE_DEVICE           UPOWER_SERVICE ".Device"
#define UPOWER_PATH_DISPLAY_DEVICE    "/org/freedesktop/UPower/devices/DisplayDevice"

int upower_initialize(void);
int upower_subscribe_signals(void);

#endif
