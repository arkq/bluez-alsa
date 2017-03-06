/*
 * BlueALSA - hfp.h
 * Copyright (c) 2016-2017 Arkadiusz Bokowy
 *               2017 Juha Kuikka
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_HFP_H_
#define BLUEALSA_HFP_H_

/* HFP codec IDs */
#define HFP_CODEC_UNDEFINED 0x00
#define HFP_CODEC_CVSD      0x01
#define HFP_CODEC_MSBC      0x02

/* AG feature flags */
#define HFP_AG_FEAT_CODEC (1 << 9)
#define HFP_AG_FEAT_ECS   (1 << 6)

/* HF feature flags */
#define HFP_HF_FEAT_CODEC (1 << 7)

#define HFP_AG_FEATURES HFP_AG_FEAT_ECS

/* Apple's extension feature flags */
#define XAPL_FEATURE_BATTERY (1 << 1)
#define XAPL_FEATURE_DOCKING (1 << 2)
#define XAPL_FEATURE_SIRI    (1 << 3)
#define XAPL_FEATURE_DENOISE (1 << 4)

#endif
