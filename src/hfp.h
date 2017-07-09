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
#define HFP_AG_FEAT_TWC    (1 << 0)
#define HFP_AG_FEAT_ECNR   (1 << 1)
#define HFP_AG_FEAT_VREC   (1 << 2)
#define HFP_AG_FEAT_RING   (1 << 3)
#define HFP_AG_FEAT_VTAG   (1 << 4)
#define HFP_AG_FEAT_REJECT (1 << 5)
#define HFP_AG_FEAT_ECS    (1 << 6)
#define HFP_AG_FEAT_ECC    (1 << 7)
#define HFP_AG_FEAT_EERC   (1 << 8)
#define HFP_AG_FEAT_CODEC  (1 << 9)
#define HFP_AG_FEAT_HFIND  (1 << 10)
#define HFP_AG_FEAT_ESOC   (1 << 11)

/* HF feature flags */
#define HFP_HF_FEAT_ECNR   (1 << 0)
#define HFP_HF_FEAT_TWC    (1 << 1)
#define HFP_HF_FEAT_CLI    (1 << 2)
#define HFP_HF_FEAT_VREC   (1 << 3)
#define HFP_HF_FEAT_VOLUME (1 << 4)
#define HFP_HF_FEAT_ECS    (1 << 5)
#define HFP_HF_FEAT_ECC    (1 << 6)
#define HFP_HF_FEAT_CODEC  (1 << 7)
#define HFP_HF_FEAT_HFIND  (1 << 8)
#define HFP_HF_FEAT_ESOC   (1 << 9)

/* Apple's extension feature flags */
#define XAPL_FEATURE_BATTERY (1 << 1)
#define XAPL_FEATURE_DOCKING (1 << 2)
#define XAPL_FEATURE_SIRI    (1 << 3)
#define XAPL_FEATURE_DENOISE (1 << 4)

/**
 * HFP Connection States */
enum hfp_state {
	HFP_DISCONNECTED,
	HFP_SLC_BRSF_SET,
	HFP_SLC_BRSF_SET_OK,
	HFP_SLC_BAC_SET_OK,
	HFP_SLC_CIND_TEST,
	HFP_SLC_CIND_TEST_OK,
	HFP_SLC_CIND_GET,
	HFP_SLC_CIND_GET_OK,
	HFP_SLC_CMER_SET_OK,
	/* Established Service Level Connection */
	HFP_SLC_CONNECTED,
	HFP_CC_BCS_SET,
	HFP_CC_BCS_SET_OK,
	/* Established Codec Connection */
	HFP_CC_CONNECTED,
	HFP_CONNECTED,
};

#endif
