/*
 * BlueALSA - io.c
 * Copyright (c) 2017 Juha Kuikka
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_HFP_H_
#define BLUEALSA_HFP_H_

/* HFP codec IDs */
#define HFP_CODEC_CVSD		1
#define HFP_CODEC_MSBC		2

/* AG Feature flags */
#define HFP_AG_FEAT_CODEC	(1 << 9)
#define HFP_AG_FEAT_ECS		(1 << 6)

/* HF feature flags */
#define HFP_HF_FEAT_CODEC	(1 << 7)

#define HFP_AG_FEATURES		 HFP_AG_FEAT_ECS

#endif /* BLUEALSA_HFP_CODECS_H_ */
