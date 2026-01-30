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

#endif
