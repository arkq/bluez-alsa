/*
 * BlueALSA - bt-codecs.h
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_SHARED_BTCODECS_H_
#define BLUEALSA_SHARED_BTCODECS_H_

#include <stdint.h>

const char *bt_codecs_a2dp_to_string(uint16_t codec);
const char *bt_codecs_hfp_to_string(uint16_t codec);

#endif
