/*
 * BlueALSA - bluetooth-asha.h
 * SPDX-FileCopyrightText: 2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 *
 * Bluetooth ASHA (Audio Streaming for Hearing Aids) specification:
 * https://source.android.com/docs/core/connect/bluetooth/asha
 *
 */

#pragma once
#ifndef BLUEALSA_SHARED_BLUETOOTH_ASHA_H_
#define BLUEALSA_SHARED_BLUETOOTH_ASHA_H_

#include <endian.h>
#include <stdint.h>

#include "defs.h"

#define ASHA_VERSION_1_0                0x01

#define ASHA_CAPABILITY_SIDE_LEFT       0
#define ASHA_CAPABILITY_SIDE_RIGHT      1

/* Support for audio over BLE Connection-Oriented Channels (CoC). */
#define ASHA_FEATURE_LE_COC_AUDIO       (1 << 0)

#define ASHA_CODEC_UNDEFINED            0
/* G.722 at 16 kHz sampling frequency. */
#define ASHA_CODEC_G722                 1

/**
 * Unique ASHA device identifier.
 *
 * It must be the same for the left and right device set but unique across
 * different device sets. */
typedef struct asha_hi_sync_id {
	uint16_t vendor_id;
	uint8_t unique_id[6];
#define ASHA_HI_SYNC_ID_INIT(v, ...) { HTOLE16(v), { __VA_ARGS__ } }
#define ASHA_HI_SYNC_ID_GET_VENDOR_ID(a) le32toh((a).vendor_id)
#define ASHA_HI_SYNC_ID_GET_UNIQUE_ID(a) (a).unique_id
} __attribute__ ((packed)) asha_hi_sync_id_t;

typedef struct asha_capabilities {
#if __BYTE_ORDER == __BIG_ENDIAN
	uint8_t side:1;
	/* This bit indicates whether the device is standalone
	 * and receives mono audio, or part of a binaural set. */
	uint8_t binaural:1;
	/* This bit indicates support for the
	 * Coordinated Set Identification Service. */
	uint8_t csis:1;
	uint8_t reserved:5;
#else
	uint8_t reserved:5;
	uint8_t csis:1;
	uint8_t binaural:1;
	uint8_t side:1;
#endif
} __attribute__ ((packed)) asha_capabilities_t;

/**
 * Payload for Service Data AD type in ASHA LE advertisement. */
typedef struct asha_service_data_payload	{
	uint8_t version;
	asha_capabilities_t caps;
	/* Four most significant bytes of ASHA device identifier. */
	uint8_t id[4];
} __attribute__ ((packed)) asha_service_data_payload_t;

/**
 * Properties exposed on BT_UUID_ASHA_PROPS GATT characteristic. */
typedef struct asha_properties {
	uint8_t version;
	asha_capabilities_t caps;
	asha_hi_sync_id_t id;
	uint8_t features;
	/* Audio render delay in milliseconds. This value is read by the ASHA
	 * client during the initial setup and can not be updated later... */
	uint16_t delay;
	uint8_t reserved[2];
	uint16_t codecs;
} __attribute__ ((packed)) asha_properties_t;

/**
 * ASHA control point characteristic opcodes. */
#define ASHA_CTRL_OP_START              0x01
#define ASHA_CTRL_OP_STOP               0x02
#define ASHA_CTRL_OP_STATUS             0x03

#define ASHA_CTRL_OP_START_AUDIO_TYPE_UNKNOWN   (1 << 0)
#define ASHA_CTRL_OP_START_AUDIO_TYPE_RINGTONE  (1 << 1)
#define ASHA_CTRL_OP_START_AUDIO_TYPE_PHONE     (1 << 2)
#define ASHA_CTRL_OP_START_AUDIO_TYPE_MEDIA     (1 << 3)

/**
 * ASHA control point characteristic START packet. */
typedef struct asha_ctrl_start {
	uint8_t codec;
	uint8_t audio_type;
	uint8_t volume;
	uint8_t status;
} __attribute__ ((packed)) asha_ctrl_start_t;

/**
 * ASHA control point characteristic STATUS packet. */
typedef struct asha_ctrl_status {
	uint8_t status;
} __attribute__ ((packed)) asha_ctrl_status_t;

/**
 * ASHA audio status point characteristic opcodes. */
#define ASHA_STATUS_OP_OK               0
#define ASHA_STATUS_OP_UNKNOWN_COMMAND -1
#define ASHA_STATUS_OP_INVALID_PARAM   -2

/**
 * Get ASHA codec ID from string representation.
 *
 * @param alias Alias of ASHA audio codec name.
 * @return ASHA audio codec ID or ASHA_CODEC_UNDEFINED in case of no match. */
uint8_t asha_codec_from_string(const char * alias);

/**
 * Convert ASHA codec ID into a human-readable string.
 *
 * @param codec ASHA audio codec ID.
 * @return Human-readable string or NULL for unknown codec. */
const char * asha_codec_to_string(uint8_t codec);

#endif
