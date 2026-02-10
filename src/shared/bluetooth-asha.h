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

#define ASHA_CAPABILITY_SIDE_LEFT       0
#define ASHA_CAPABILITY_SIDE_RIGHT      1

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
