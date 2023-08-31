/*
 * BlueALSA - ble-midi.h
 * Copyright (c) 2016-2023 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_BLEMIDI_H_
#define BLUEALSA_BLEMIDI_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ble_midi_dec {

	/* timestamp */
	unsigned int ts;
	/* decoded MIDI message */
	uint8_t *buffer;
	/* length of the decoded message */
	size_t len;

	/* storage for decoded MIDI message */
	uint8_t buffer_midi[8];
	size_t buffer_midi_len;
	/* timestamp of the system exclusive message start */
	unsigned int ts_sys;
	/* storage for decoded system exclusive message */
	uint8_t buffer_sys[256];
	size_t buffer_sys_len;

	/* most significant 7 bits of the timestamp */
	unsigned int ts_high;
	/* lastly seen status byte */
	uint8_t status;
	/* system exclusive is being parsed */
	bool status_sys;
	/* add status byte to the running status */
	bool status_restore;
	/* current parsing position */
	size_t current_len;

};

struct ble_midi_enc {

	/* The MTU of the BLE link. This structure member shall be set before
	 * calling the ble_midi_encode() function. */
	size_t mtu;

	/* encoded BLE-MIDI message */
	uint8_t buffer[512];
	/* length of the encoded message */
	size_t len;

	/* current timestamp */
	unsigned int ts;
	/* current encoding position */
	size_t current_len;

};

int ble_midi_decode(struct ble_midi_dec *bmd, const uint8_t *data, size_t len);
int ble_midi_encode(struct ble_midi_enc *bme, const uint8_t *data, size_t len);
int ble_midi_encode_set_mtu(struct ble_midi_enc *bme, size_t mtu);

#endif
