/*
 * BlueALSA - bluetooth-midi.h
 * SPDX-FileCopyrightText: 2023-2025 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BLUEALSA_SHARED_BLUETOOTH_MIDI_H_
#define BLUEALSA_SHARED_BLUETOOTH_MIDI_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdbool.h>
#include <stdint.h>
#include <sys/param.h>
#include <time.h>

/**
 * Structure used for BLE-MIDI encoding. */
struct ble_midi_enc {

	/* The MTU of the BLE link. This structure member shall be set before
	 * calling the ble_midi_encode() function. */
	size_t mtu;

	/* Encoded BLE-MIDI message. */
	uint8_t buffer[512];
	/* Length of the encoded message. */
	size_t len;

	/* Current encoding position. */
	size_t current_len;

};

/**
 * Initialize BLE-MIDI encoder. */
void ble_midi_encode_init(
		struct ble_midi_enc * enc);

/**
 * Free BLE-MIDI encoder resources. */
void ble_midi_encode_free(
		struct ble_midi_enc * enc);

/**
 * Encode BLE-MIDI packet.
 *
 * It is possible that a single MIDI system exclusive message will not fit
 * into the MTU of the BLE link. In such case, this function will return 1
 * and the caller should call this function again with the same MIDI message.
 * The encoder structure should not be modified between consecutive calls.
 *
 * @param enc Initialized BLE-MIDI encoder structure.
 * @param data Single MIDI message.
 * @param len Length of the MIDI message data.
 * @return On success, this function returns 0. If the BLE-MIDI packet has to
 *   be split into multiple packets, 1 is returned. On error, -1 is returned. */
int ble_midi_encode(
		struct ble_midi_enc * enc,
		const uint8_t * data,
		size_t len);

/**
 * Set BLE-MIDI encoder MTU. */
static inline void ble_midi_encode_set_mtu(
		struct ble_midi_enc * enc,
		size_t mtu) {
	enc->mtu = MIN(mtu, sizeof(enc->buffer));
}

/**
 * Structure used for BLE-MIDI decoding. */
struct ble_midi_dec {

	/* Decoded message timestamp. */
	struct timespec ts;
	/* Decoded MIDI message. */
	uint8_t * buffer;
	/* Length of the decoded message. */
	size_t len;

	/* Storage for decoded MIDI message. */
	uint8_t buffer_midi[8];
	size_t buffer_midi_len;
	/* Storage for decoded system exclusive message. */
	uint8_t * buffer_sys;
	size_t buffer_sys_size;
	size_t buffer_sys_len;

	/* Reconstructed timestamp value. */
	unsigned int ts_high_low;
	/* Previous timestamp-high value (most significant 6 bits). */
	unsigned char ts_high;
	/* Previous timestamp-low value (least significant 7 bits). */
	unsigned char ts_low;
	/* Lastly seen status byte. */
	uint8_t status;
	/* System exclusive is being parsed. */
	bool status_sys;
	/* Add status byte to the running status. */
	bool status_restore;
	/* Current parsing position. */
	size_t current_len;

	/* Initialization host time. */
	struct timespec ts0;

};

/**
 * Initialize BLE-MIDI decoder. */
void ble_midi_decode_init(
		struct ble_midi_dec * dec);

/**
 * Free BLE-MIDI decoder resources. */
void ble_midi_decode_free(
		struct ble_midi_dec * dec);

/**
 * Decode BLE-MIDI packet.
 *
 * Before decoding next BLE-MIDI packet, this function should be called until
 * it returns 0 or -1. Alternatively, caller can set the decoder structure to
 * all-zeroes, which will reset the decoding state.
 *
 * @param dec Initialized BLE-MIDI decoder structure.
 * @param data BLE-MIDI packet data.
 * @param len Length of the packet data.
 * @return On success, in case when at least one full MIDI message was decoded,
 *   this function returns 1. If the BLE-MIDI packet does not contain any more
 *   (complete) MIDI message, 0 is returned. On error, -1 is returned. */
int ble_midi_decode(
		struct ble_midi_dec * dec,
		const uint8_t * data,
		size_t len);

#endif
