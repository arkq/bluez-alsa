/*
 * BlueALSA - bluez.h
 * Copyright (c) 2016 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_BLUEZ_H_
#define BLUEALSA_BLUEZ_H_

#if HAVE_CONFIG_H
# include "config.h"
#endif

/* List of Bluetoth audio profiles. */
#define BLUETOOTH_UUID_A2DP_SOURCE "0000110A-0000-1000-8000-00805F9B34FB"
#define BLUETOOTH_UUID_A2DP_SINK   "0000110B-0000-1000-8000-00805F9B34FB"
#define BLUETOOTH_UUID_HSP_HS      "00001108-0000-1000-8000-00805F9B34FB"
#define BLUETOOTH_UUID_HSP_AG      "00001112-0000-1000-8000-00805F9B34FB"
#define BLUETOOTH_UUID_HFP_HF      "0000111E-0000-1000-8000-00805F9B34FB"
#define BLUETOOTH_UUID_HFP_AG      "0000111F-0000-1000-8000-00805F9B34FB"

enum bluetooth_profile {
	BLUETOOTH_PROFILE_NULL = 0,
	BLUETOOTH_PROFILE_A2DP_SOURCE,
	BLUETOOTH_PROFILE_A2DP_SINK,
	BLUETOOTH_PROFILE_HSP_HS,
	BLUETOOTH_PROFILE_HSP_AG,
	BLUETOOTH_PROFILE_HFP_HF,
	BLUETOOTH_PROFILE_HFP_AG,
};

/* List of media endpoints registered by us. */
#define BLUEZ_ENDPOINT_A2DP_SBC_SOURCE    "/MediaEndpoint/A2DPSource"
#define BLUEZ_ENDPOINT_A2DP_SBC_SINK      "/MediaEndpoint/A2DPSink"
#define BLUEZ_ENDPOINT_A2DP_MPEG12_SOURCE "/MediaEndpoint/A2DP_MPEG12_Source"
#define BLUEZ_ENDPOINT_A2DP_MPEG12_SINK   "/MediaEndpoint/A2DP_MPEG12_Sink"
#define BLUEZ_ENDPOINT_A2DP_MPEG24_SOURCE "/MediaEndpoint/A2DP_MPEG24_Source"
#define BLUEZ_ENDPOINT_A2DP_MPEG24_SINK   "/MediaEndpoint/A2DP_MPEG24_Sink"
#define BLUEZ_ENDPOINT_A2DP_ATRAC_SOURCE  "/MediaEndpoint/A2DP_ATRAC_Source"
#define BLUEZ_ENDPOINT_A2DP_ATRAC_SINK    "/MediaEndpoint/A2DP_ATRAC_Sink"

/* List of profiles registered by us. */
#define BLUEZ_PROFILE_HSP_HS "/Profile/HSPHeadset"
#define BLUEZ_PROFILE_HSP_AG "/Profile/HSPAudioGateway"
#define BLUEZ_PROFILE_HFP_HF "/Profile/HFPHandsFree"
#define BLUEZ_PROFILE_HFP_AG "/Profile/HFPAudioGateway"

int bluez_register_a2dp(void);
int bluez_register_hfp(void);
int bluez_subscribe_signals(void);

#endif
