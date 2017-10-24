/*
 * BlueALSA - bluez-a2dp.h
 * Copyright (c) 2016-2017 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_BLUEZA2DP_H_
#define BLUEALSA_BLUEZA2DP_H_

#if HAVE_CONFIG_H
# include "config.h"
#endif

#include "a2dp-codecs.h"

const a2dp_sbc_t bluez_a2dp_sbc;
#if ENABLE_MP3
const a2dp_mpeg_t bluez_a2dp_mpeg;
#endif
#if ENABLE_AAC
const a2dp_aac_t bluez_a2dp_aac;
#endif
#if ENABLE_APTX
const a2dp_aptx_t bluez_a2dp_aptx;
#endif

#endif
