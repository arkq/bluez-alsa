/*
 * BlueALSA - hfp.h
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 * Copyright (c) 2017 Juha Kuikka
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_HFP_H_
#define BLUEALSA_HFP_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdint.h>
#include <sys/types.h>

/* HFP codec IDs */
#define HFP_CODEC_UNDEFINED 0x00
#define HFP_CODEC_CVSD      0x01
#define HFP_CODEC_MSBC      0x02
#define HFP_CODEC_LC3_SWB   0x03

/**
 * HSP/HFP volume gain range */
#define HFP_VOLUME_GAIN_MIN 0
#define HFP_VOLUME_GAIN_MAX 15

/**
 * SDP AG feature flags */
#define SDP_HFP_AG_FEAT_TWC    (1 << 0) /* three-way calling */
#define SDP_HFP_AG_FEAT_ECNR   (1 << 1) /* EC and/or NR function */
#define SDP_HFP_AG_FEAT_VR     (1 << 2) /* voice recognition function */
#define SDP_HFP_AG_FEAT_RING   (1 << 3) /* in-band ring tone capability */
#define SDP_HFP_AG_FEAT_VTAG   (1 << 4) /* attach a number to a voice tag */
#define SDP_HFP_AG_FEAT_WBS    (1 << 5) /* wide band speech */
#define SDP_HFP_AG_FEAT_EVR    (1 << 6) /* enhanced voice recognition status */
#define SDP_HFP_AG_FEAT_VR_TXT (1 << 7) /* voice recognition text */
#define SDP_HFP_AG_FEAT_SWB    (1 << 8) /* super wide band speech */

/**
 * SDP HF feature flags */
#define SDP_HFP_HF_FEAT_ECNR   (1 << 0) /* EC and/or NR function */
#define SDP_HFP_HF_FEAT_TWC    (1 << 1) /* call waiting or three-way calling */
#define SDP_HFP_HF_FEAT_CLI    (1 << 2) /* CLI presentation capability */
#define SDP_HFP_HF_FEAT_VR     (1 << 3) /* voice recognition activation */
#define SDP_HFP_HF_FEAT_VOLUME (1 << 4) /* remote audio volume control */
#define SDP_HFP_HF_FEAT_WBS    (1 << 5) /* wide band speech */
#define SDP_HFP_HF_FEAT_EVR    (1 << 6) /* enhanced voice recognition status */
#define SDP_HFP_HF_FEAT_VR_TXT (1 << 7) /* voice recognition text */
#define SDP_HFP_HF_FEAT_SWB    (1 << 8) /* super wide band speech */

/**
 * AG feature flags */
#define HFP_AG_FEAT_3WC    (1 << 0)  /* three-way calling */
#define HFP_AG_FEAT_ECNR   (1 << 1)  /* EC and/or NR function */
#define HFP_AG_FEAT_VOICE  (1 << 2)  /* voice recognition function */
#define HFP_AG_FEAT_RING   (1 << 3)  /* in-band ring tone capability */
#define HFP_AG_FEAT_VTAG   (1 << 4)  /* attach a number to a voice tag */
#define HFP_AG_FEAT_REJECT (1 << 5)  /* ability to reject a call */
#define HFP_AG_FEAT_ECS    (1 << 6)  /* enhanced call status */
#define HFP_AG_FEAT_ECC    (1 << 7)  /* enhanced call control */
#define HFP_AG_FEAT_EERC   (1 << 8)  /* extended error result codes */
#define HFP_AG_FEAT_CODEC  (1 << 9)  /* codec negotiation */
#define HFP_AG_FEAT_HF_IND (1 << 10) /* HF indicators */
#define HFP_AG_FEAT_ESCO   (1 << 11) /* enhanced SCO S4 settings supported */
#define HFP_AG_FEAT_EVRS   (1 << 12) /* enhanced voice recognition status */
#define HFP_AG_FEAT_VR_TXT (1 << 13) /* voice recognition text */

/**
 * HF feature flags */
#define HFP_HF_FEAT_ECNR   (1 << 0)  /* EC and/or NR function */
#define HFP_HF_FEAT_3WC    (1 << 1)  /* three-way calling */
#define HFP_HF_FEAT_CLI    (1 << 2)  /* CLI presentation capability */
#define HFP_HF_FEAT_VOICE  (1 << 3)  /* voice recognition activation */
#define HFP_HF_FEAT_VOLUME (1 << 4)  /* remote volume control */
#define HFP_HF_FEAT_ECS    (1 << 5)  /* enhanced call status */
#define HFP_HF_FEAT_ECC    (1 << 6)  /* enhanced call control */
#define HFP_HF_FEAT_CODEC  (1 << 7)  /* codec negotiation */
#define HFP_HF_FEAT_HF_IND (1 << 8)  /* HF indicators */
#define HFP_HF_FEAT_ESCO   (1 << 9)  /* enhanced SCO S4 settings supported */
#define HFP_HF_FEAT_EVRS   (1 << 10) /* enhanced voice recognition status */
#define HFP_HF_FEAT_VR_TXT (1 << 11) /* voice recognition text */

/**
 * Apple's extension feature flags */
#define XAPL_FEATURE_BATTERY (1 << 1)
#define XAPL_FEATURE_DOCKING (1 << 2)
#define XAPL_FEATURE_SIRI    (1 << 3)
#define XAPL_FEATURE_DENOISE (1 << 4)

/**
 * HFP Service Level Connection States */
enum __attribute__ ((packed)) hfp_slc_state {
	HFP_DISCONNECTED,
	HFP_SLC_BRSF_SET,
	HFP_SLC_BRSF_SET_OK,
	HFP_SLC_BAC_SET_OK,
	HFP_SLC_CIND_TEST,
	HFP_SLC_CIND_TEST_OK,
	HFP_SLC_CIND_GET,
	HFP_SLC_CIND_GET_OK,
	HFP_SLC_CMER_SET_OK,
	HFP_SLC_CONNECTED,
};

/**
 * Initial BT accessory setup */
enum __attribute__ ((packed)) hfp_setup {
	HFP_SETUP_GAIN_MIC,
	HFP_SETUP_GAIN_SPK,
	HFP_SETUP_ACCESSORY_XAPL,
	HFP_SETUP_ACCESSORY_BATT,
	HFP_SETUP_SELECT_CODEC,
	HFP_SETUP_COMPLETE,
};

/**
 * HFP Indicators */
enum __attribute__ ((packed)) hfp_ind {
	HFP_IND_NULL = 0,
	HFP_IND_SERVICE,
	HFP_IND_CALL,
	HFP_IND_CALLSETUP,
	HFP_IND_CALLHELD,
	HFP_IND_SIGNAL,
	HFP_IND_ROAM,
	HFP_IND_BATTCHG,
	__HFP_IND_MAX
};

/* no Home/Roam network available */
#define HFP_IND_SERVICE_NONE        0
/* Home/Roam network available */
#define HFP_IND_SERVICE_ACTIVE      1
/* no calls in progress */
#define HFP_IND_CALL_NONE           0
/* at least one call is in progress */
#define HFP_IND_CALL_ACTIVE         1
/* currently not in call set up */
#define HFP_IND_CALLSETUP_NONE      0
/* an incoming call process ongoing */
#define HFP_IND_CALLSETUP_IN        1
/* an outgoing call set up is ongoing */
#define HFP_IND_CALLSETUP_OUT       2
/* remote party being alerted in an outgoing call */
#define HFP_IND_CALLSETUP_OUT_ALERT 3
/* no calls held */
#define HFP_IND_CALLHELD_NONE       0
/* call on hold with other active call */
#define HFP_IND_CALLHELD_SWAP       1
/* call on hold, no active call */
#define HFP_IND_CALLHELD_HOLD       2
/* roaming is not active */
#define HFP_IND_ROAM_NONE           0
/* a roaming is active */
#define HFP_IND_ROAM_ACTIVE         1

ssize_t hfp_ag_features_to_strings(uint32_t features, const char **out, size_t size);
ssize_t hfp_hf_features_to_strings(uint32_t features, const char **out, size_t size);

uint8_t hfp_codec_id_from_string(const char *alias);
const char *hfp_codec_id_to_string(uint8_t codec_id);

#endif
