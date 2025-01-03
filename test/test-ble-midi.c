/*
 * test-ble-midi.c
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <check.h>

#include "ble-midi.h"
#include "shared/rt.h"

#include "inc/check.inc"

CK_START_TEST(test_ble_midi_decode_init) {

	const uint8_t data[] = { 0x8F, 0xA0, 0xFF };

	struct ble_midi_dec bmd;
	ble_midi_decode_init(&bmd);

	ck_assert_int_eq(ble_midi_decode(&bmd, data, sizeof(data)), 1);
	/* This test checks whether the timestamp equals to the current time
	 * minus the initialization time. Since this test depends on timing
	 * we can not be very strict (0 ms) here. */
	ck_assert_uint_lt(timespec2ms(&bmd.ts), 5);

} CK_END_TEST

CK_START_TEST(test_ble_midi_decode_single) {

	const uint8_t data[] = { 0x80, 0x81, 0xC0, 0x42 };
	const uint8_t midi[] = { 0xC0, 0x42 };

	struct ble_midi_dec bmd;
	ble_midi_decode_init(&bmd);

	ck_assert_int_eq(ble_midi_decode(&bmd, data, sizeof(data)), 1);
	ck_assert_int_eq(ble_midi_decode(&bmd, data, sizeof(data)), 0);

	ck_assert_uint_eq(timespec2ms(&bmd.ts), 1);
	ck_assert_uint_eq(bmd.len, sizeof(midi));
	ck_assert_mem_eq(bmd.buffer, midi, sizeof(midi));

} CK_END_TEST

CK_START_TEST(test_ble_midi_decode_multiple) {

	const uint8_t data1[] = { 0x80, 0x81, 0x90, 0x40, 0x7f };
	const uint8_t data2[] = { 0x80, 0x82, 0xA0, 0x40, 0x7f };
	const uint8_t midi1[] = { 0x90, 0x40, 0x7f };
	const uint8_t midi2[] = { 0xA0, 0x40, 0x7f };

	struct ble_midi_dec bmd;
	ble_midi_decode_init(&bmd);

	ck_assert_int_eq(ble_midi_decode(&bmd, data1, sizeof(data1)), 1);
	ck_assert_int_eq(ble_midi_decode(&bmd, data1, sizeof(data1)), 0);

	ck_assert_uint_eq(timespec2ms(&bmd.ts), 1);
	ck_assert_uint_eq(bmd.len, sizeof(midi1));
	ck_assert_mem_eq(bmd.buffer, midi1, sizeof(midi1));

	ck_assert_int_eq(ble_midi_decode(&bmd, data2, sizeof(data2)), 1);
	ck_assert_int_eq(ble_midi_decode(&bmd, data2, sizeof(data2)), 0);

	ck_assert_uint_eq(timespec2ms(&bmd.ts), 2);
	ck_assert_uint_eq(bmd.len, sizeof(midi2));
	ck_assert_mem_eq(bmd.buffer, midi2, sizeof(midi2));

} CK_END_TEST

CK_START_TEST(test_ble_midi_decode_invalid_header) {

	const uint8_t data[] = { 0x10, 0x80, 0x90, 0x40, 0x7f };

	struct ble_midi_dec bmd;
	ble_midi_decode_init(&bmd);

	ck_assert_int_eq(ble_midi_decode(&bmd, data, sizeof(data)), -1);

} CK_END_TEST

CK_START_TEST(test_ble_midi_decode_invalid_status) {

	const uint8_t data[] = { 0x80, 0x80, 0x40, 0x40, 0x7f };

	struct ble_midi_dec bmd;
	ble_midi_decode_init(&bmd);

	ck_assert_int_eq(ble_midi_decode(&bmd, data, sizeof(data)), -1);

} CK_END_TEST

CK_START_TEST(test_ble_midi_decode_invalid_interleaved_real_time) {

	const uint8_t data[] = { 0x80, 0x80, 0x90, 0x40, 0xF8, 0x7f };

	struct ble_midi_dec bmd;
	ble_midi_decode_init(&bmd);

	ck_assert_int_eq(ble_midi_decode(&bmd, data, sizeof(data)), -1);

} CK_END_TEST

CK_START_TEST(test_ble_midi_decode_single_joined) {

	const uint8_t data[] = { 0x80, 0x81, 0x90, 0x40, 0x7f, 0x81, 0xE0, 0x10, 0x42 };
	const uint8_t midi1[] = { 0x90, 0x40, 0x7f };
	const uint8_t midi2[] = { 0xE0, 0x10, 0x42 };

	struct ble_midi_dec bmd;
	ble_midi_decode_init(&bmd);

	ck_assert_int_eq(ble_midi_decode(&bmd, data, sizeof(data)), 1);
	ck_assert_uint_eq(timespec2ms(&bmd.ts), 1);
	ck_assert_uint_eq(bmd.len, sizeof(midi1));
	ck_assert_mem_eq(bmd.buffer, midi1, sizeof(midi1));

	ck_assert_int_eq(ble_midi_decode(&bmd, data, sizeof(data)), 1);
	ck_assert_uint_eq(timespec2ms(&bmd.ts), 1);
	ck_assert_uint_eq(bmd.len, sizeof(midi2));
	ck_assert_mem_eq(bmd.buffer, midi2, sizeof(midi2));

} CK_END_TEST

CK_START_TEST(test_ble_midi_decode_single_real_time) {

	const uint8_t data[] = { 0x80, 0x81, 0xFF };
	const uint8_t midi[] = { 0xFF };

	struct ble_midi_dec bmd;
	ble_midi_decode_init(&bmd);

	ck_assert_int_eq(ble_midi_decode(&bmd, data, sizeof(data)), 1);
	ck_assert_uint_eq(timespec2ms(&bmd.ts), 1);
	ck_assert_uint_eq(bmd.len, sizeof(midi));
	ck_assert_mem_eq(bmd.buffer, midi, sizeof(midi));

} CK_END_TEST

CK_START_TEST(test_ble_midi_decode_multiple_real_time) {

	const uint8_t data1[] = { 0x80, 0x81, 0xF3, 0x01 };
	const uint8_t data2[] = { 0x80, 0x81, 0xF2, 0x7F, 0x7F };
	const uint8_t midi1[] = { 0xF3, 0x01 };
	const uint8_t midi2[] = { 0xF2, 0x7F, 0x7F };

	struct ble_midi_dec bmd;
	ble_midi_decode_init(&bmd);

	ck_assert_int_eq(ble_midi_decode(&bmd, data1, sizeof(data1)), 1);
	ck_assert_int_eq(ble_midi_decode(&bmd, data1, sizeof(data1)), 0);
	ck_assert_uint_eq(timespec2ms(&bmd.ts), 1);
	ck_assert_uint_eq(bmd.len, sizeof(midi1));
	ck_assert_mem_eq(bmd.buffer, midi1, sizeof(midi1));

	ck_assert_int_eq(ble_midi_decode(&bmd, data2, sizeof(data2)), 1);
	ck_assert_int_eq(ble_midi_decode(&bmd, data2, sizeof(data2)), 0);
	ck_assert_uint_eq(timespec2ms(&bmd.ts), 1);
	ck_assert_uint_eq(bmd.len, sizeof(midi2));
	ck_assert_mem_eq(bmd.buffer, midi2, sizeof(midi2));

} CK_END_TEST

CK_START_TEST(test_ble_midi_decode_single_system_exclusive) {

	const uint8_t data[] = { 0x80, 0x81, 0xF0, 0x01, 0x02, 0x81, 0xF7 };
	const uint8_t midi[] = { 0xF0, 0x01, 0x02, 0xF7 };

	struct ble_midi_dec bmd;
	ble_midi_decode_init(&bmd);

	ck_assert_int_eq(ble_midi_decode(&bmd, data, sizeof(data)), 1);
	ck_assert_int_eq(ble_midi_decode(&bmd, data, sizeof(data)), 0);

	ck_assert_uint_eq(timespec2ms(&bmd.ts), 1);
	ck_assert_uint_eq(bmd.len, sizeof(midi));
	ck_assert_mem_eq(bmd.buffer, midi, sizeof(midi));

	ble_midi_decode_free(&bmd);

} CK_END_TEST

CK_START_TEST(test_ble_midi_decode_multiple_system_exclusive) {

	const uint8_t data1[] = { 0x80, 0x81, 0xF0, 0x01, 0x02, 0x03 };
	const uint8_t data2[] = { 0x80, 0x04, 0x05, 0x81, 0xF7 };
	const uint8_t midi[] = { 0xF0, 0x01, 0x02, 0x03, 0x04, 0x05, 0xF7 };

	struct ble_midi_dec bmd;
	ble_midi_decode_init(&bmd);

	ck_assert_int_eq(ble_midi_decode(&bmd, data1, sizeof(data1)), 0);
	ck_assert_int_eq(ble_midi_decode(&bmd, data2, sizeof(data2)), 1);
	ck_assert_int_eq(ble_midi_decode(&bmd, data2, sizeof(data2)), 0);

	ck_assert_uint_eq(timespec2ms(&bmd.ts), 1);
	ck_assert_uint_eq(bmd.len, sizeof(midi));
	ck_assert_mem_eq(bmd.buffer, midi, sizeof(midi));

	ble_midi_decode_free(&bmd);

} CK_END_TEST

CK_START_TEST(test_ble_midi_decode_multiple_system_exclusive_2) {

	const uint8_t data1[] = { 0x80, 0x81, 0xF0, 0x01, 0x02, 0x03 };
	const uint8_t data2[] = { 0x80, 0x81, 0xF7 };
	const uint8_t midi[] = { 0xF0, 0x01, 0x02, 0x03, 0xF7 };

	struct ble_midi_dec bmd;
	ble_midi_decode_init(&bmd);

	ck_assert_int_eq(ble_midi_decode(&bmd, data1, sizeof(data1)), 0);
	ck_assert_int_eq(ble_midi_decode(&bmd, data2, sizeof(data2)), 1);
	ck_assert_int_eq(ble_midi_decode(&bmd, data2, sizeof(data2)), 0);

	ck_assert_uint_eq(timespec2ms(&bmd.ts), 1);
	ck_assert_uint_eq(bmd.len, sizeof(midi));
	ck_assert_mem_eq(bmd.buffer, midi, sizeof(midi));

	ble_midi_decode_free(&bmd);

} CK_END_TEST

CK_START_TEST(test_ble_midi_decode_multiple_system_exclusive_3) {

	struct ble_midi_dec bmd;
	ble_midi_decode_init(&bmd);

	const uint8_t data1[] = { 0x80, 0x81, 0xF0, 0x01, 0x02, 0x03 };
	uint8_t data2[2 + 512] = { 0x80, 0x81, /* ... */ };
	memset(data2 + 2, 0x77, sizeof(data2) - 2);
	const uint8_t data3[] = { 0x80, 0x81, 0xF7 };
	uint8_t midi[1 + 3 + 512 + 1] = { 0xF0, 0x01, 0x02, 0x03, /* ... */ 0xF7 };
	memset(midi + 4, 0x77, sizeof(midi) - 4);
	midi[sizeof(midi) - 1] = 0xF7;

	ck_assert_int_eq(ble_midi_decode(&bmd, data1, sizeof(data1)), 0);
	ck_assert_int_eq(ble_midi_decode(&bmd, data2, sizeof(data2)), 0);
	ck_assert_int_eq(ble_midi_decode(&bmd, data3, sizeof(data3)), 1);
	ck_assert_int_eq(ble_midi_decode(&bmd, data3, sizeof(data3)), 0);

	ck_assert_uint_eq(timespec2ms(&bmd.ts), 1);
	ck_assert_uint_eq(bmd.len, sizeof(midi));
	ck_assert_mem_eq(bmd.buffer, midi, sizeof(midi));

	ble_midi_decode_free(&bmd);

} CK_END_TEST

CK_START_TEST(test_ble_midi_decode_invalid_system_exclusive) {

	const uint8_t data[] = { 0x80, 0x80, 0xF0, 0x01, 0x80 };

	struct ble_midi_dec bmd;
	ble_midi_decode_init(&bmd);

	ck_assert_int_eq(ble_midi_decode(&bmd, data, sizeof(data)), -1);

	ble_midi_decode_free(&bmd);

} CK_END_TEST

CK_START_TEST(test_ble_midi_decode_single_running_status) {

	/* Data:
	 * - full MIDI message (note on)
	 * - running status MIDI message with timestamp byte
	 * - running status MIDI message without timestamp byte */
	const uint8_t data[] = { 0x80, 0x81, 0x90, 0x40, 0x7f, 0x82, 0x41, 0x7f, 0x42, 0x7f };
	const uint8_t midi1[] = { 0x90, 0x40, 0x7f };
	const uint8_t midi2[] = { 0x41, 0x7f };
	const uint8_t midi3[] = { 0x42, 0x7f };

	struct ble_midi_dec bmd;
	ble_midi_decode_init(&bmd);

	ck_assert_int_eq(ble_midi_decode(&bmd, data, sizeof(data)), 1);
	ck_assert_uint_eq(timespec2ms(&bmd.ts), 1);
	ck_assert_uint_eq(bmd.len, sizeof(midi1));
	ck_assert_mem_eq(bmd.buffer, midi1, sizeof(midi1));

	ck_assert_int_eq(ble_midi_decode(&bmd, data, sizeof(data)), 1);
	ck_assert_uint_eq(timespec2ms(&bmd.ts), 2);
	ck_assert_uint_eq(bmd.len, sizeof(midi2));
	ck_assert_mem_eq(bmd.buffer, midi2, sizeof(midi2));

	ck_assert_int_eq(ble_midi_decode(&bmd, data, sizeof(data)), 1);
	ck_assert_uint_eq(timespec2ms(&bmd.ts), 2);
	ck_assert_uint_eq(bmd.len, sizeof(midi3));
	ck_assert_mem_eq(bmd.buffer, midi3, sizeof(midi3));

} CK_END_TEST

CK_START_TEST(test_ble_midi_decode_single_running_status_with_real_time) {

	/* Data:
	 * - full MIDI message (note on)
	 * - system real-time MIDI message with timestamp byte
	 * - running status MIDI message with timestamp byte */
	const uint8_t data[] = { 0x80, 0x81, 0x90, 0x40, 0x7f, 0x82, 0xF8, 0x83, 0x41, 0x7f };
	const uint8_t midi1[] = { 0x90, 0x40, 0x7f };
	const uint8_t midi2[] = { 0xF8 };
	const uint8_t midi3[] = { 0x41, 0x7f };

	struct ble_midi_dec bmd;
	ble_midi_decode_init(&bmd);

	ck_assert_int_eq(ble_midi_decode(&bmd, data, sizeof(data)), 1);
	ck_assert_uint_eq(timespec2ms(&bmd.ts), 1);
	ck_assert_uint_eq(bmd.len, sizeof(midi1));
	ck_assert_mem_eq(bmd.buffer, midi1, sizeof(midi1));

	ck_assert_int_eq(ble_midi_decode(&bmd, data, sizeof(data)), 1);
	ck_assert_uint_eq(timespec2ms(&bmd.ts), 2);
	ck_assert_uint_eq(bmd.len, sizeof(midi2));
	ck_assert_mem_eq(bmd.buffer, midi2, sizeof(midi2));

	ck_assert_int_eq(ble_midi_decode(&bmd, data, sizeof(data)), 1);
	ck_assert_uint_eq(timespec2ms(&bmd.ts), 3);
	ck_assert_uint_eq(bmd.len, sizeof(midi3));
	ck_assert_mem_eq(bmd.buffer, midi3, sizeof(midi3));

} CK_END_TEST

CK_START_TEST(test_ble_midi_decode_single_running_status_with_common) {

	/* Data:
	 * - full MIDI message (note on)
	 * - system common MIDI message with timestamp byte
	 * - running status MIDI message with timestamp byte */
	const uint8_t data[] = { 0x80, 0x81, 0x90, 0x40, 0x7f, 0x82, 0xF1, 0x00, 0x83, 0x41, 0x7f };
	const uint8_t midi1[] = { 0x90, 0x40, 0x7f };
	const uint8_t midi2[] = { 0xF1, 0x00 };
	const uint8_t midi3[] = { 0x90, 0x41, 0x7f };

	struct ble_midi_dec bmd;
	ble_midi_decode_init(&bmd);

	ck_assert_int_eq(ble_midi_decode(&bmd, data, sizeof(data)), 1);
	ck_assert_uint_eq(timespec2ms(&bmd.ts), 1);
	ck_assert_uint_eq(bmd.len, sizeof(midi1));
	ck_assert_mem_eq(bmd.buffer, midi1, sizeof(midi1));

	ck_assert_int_eq(ble_midi_decode(&bmd, data, sizeof(data)), 1);
	ck_assert_uint_eq(timespec2ms(&bmd.ts), 2);
	ck_assert_uint_eq(bmd.len, sizeof(midi2));
	ck_assert_mem_eq(bmd.buffer, midi2, sizeof(midi2));

	ck_assert_int_eq(ble_midi_decode(&bmd, data, sizeof(data)), 1);
	ck_assert_uint_eq(timespec2ms(&bmd.ts), 3);
	ck_assert_uint_eq(bmd.len, sizeof(midi3));
	ck_assert_mem_eq(bmd.buffer, midi3, sizeof(midi3));

} CK_END_TEST

CK_START_TEST(test_ble_midi_decode_single_timestamp_overflow) {

	/* Data:
	 * - full MIDI message (note on)
	 * - full MIDI message (note on) with low-timestamp overflow/wrap */
	const uint8_t data1[] = { 0x80, 0x8F, 0x90, 0x40, 0x7f, 0x88, 0x91, 0x40, 0x7f };
	const uint8_t midi1[] = { 0x90, 0x40, 0x7f };
	const uint8_t midi2[] = { 0x91, 0x40, 0x7f };

	struct ble_midi_dec bmd;
	ble_midi_decode_init(&bmd);

	ck_assert_int_eq(ble_midi_decode(&bmd, data1, sizeof(data1)), 1);
	ck_assert_uint_eq(timespec2ms(&bmd.ts), 15);
	ck_assert_uint_eq(bmd.len, sizeof(midi1));
	ck_assert_mem_eq(bmd.buffer, midi1, sizeof(midi1));

	ck_assert_int_eq(ble_midi_decode(&bmd, data1, sizeof(data1)), 1);
	ck_assert_int_eq(ble_midi_decode(&bmd, data1, sizeof(data1)), 0);
	ck_assert_uint_eq(timespec2ms(&bmd.ts), 136);
	ck_assert_uint_eq(bmd.len, sizeof(midi2));
	ck_assert_mem_eq(bmd.buffer, midi2, sizeof(midi2));

} CK_END_TEST

CK_START_TEST(test_ble_midi_decode_multiple_running_status) {

	const uint8_t data1[] = { 0x80, 0x81, 0x90, 0x40, 0x7f };
	const uint8_t data2[] = { 0x80, 0x82, 0x41, 0x7f };
	const uint8_t data3[] = { 0x80, 0x42, 0x7f };
	const uint8_t midi1[] = { 0x90, 0x40, 0x7f };
	const uint8_t midi2[] = { 0x41, 0x7f };
	const uint8_t midi3[] = { 0x42, 0x7f };

	struct ble_midi_dec bmd;
	ble_midi_decode_init(&bmd);

	ck_assert_int_eq(ble_midi_decode(&bmd, data1, sizeof(data1)), 1);
	ck_assert_int_eq(ble_midi_decode(&bmd, data1, sizeof(data1)), 0);
	ck_assert_uint_eq(timespec2ms(&bmd.ts), 1);
	ck_assert_uint_eq(bmd.len, sizeof(midi1));
	ck_assert_mem_eq(bmd.buffer, midi1, sizeof(midi1));

	ck_assert_int_eq(ble_midi_decode(&bmd, data2, sizeof(data2)), 1);
	ck_assert_int_eq(ble_midi_decode(&bmd, data2, sizeof(data2)), 0);
	ck_assert_uint_eq(timespec2ms(&bmd.ts), 2);
	ck_assert_uint_eq(bmd.len, sizeof(midi2));
	ck_assert_mem_eq(bmd.buffer, midi2, sizeof(midi2));

	ck_assert_int_eq(ble_midi_decode(&bmd, data3, sizeof(data3)), 1);
	ck_assert_int_eq(ble_midi_decode(&bmd, data3, sizeof(data3)), 0);
	ck_assert_uint_eq(timespec2ms(&bmd.ts), 2);
	ck_assert_uint_eq(bmd.len, sizeof(midi3));
	ck_assert_mem_eq(bmd.buffer, midi3, sizeof(midi3));

} CK_END_TEST

CK_START_TEST(test_ble_midi_encode_no_mtu) {

	const uint8_t midi[] = { 0x90, 0x40, 0x7f };

	struct ble_midi_enc bme;
	ble_midi_encode_init(&bme);

	ck_assert_int_eq(ble_midi_encode(&bme, midi, sizeof(midi)), -1);
	ck_assert_uint_eq(errno, EINVAL);

} CK_END_TEST

CK_START_TEST(test_ble_midi_encode_single) {

	const uint8_t midi[] = { 0x90, 0x40, 0x7f };

	struct ble_midi_enc bme;
	ble_midi_encode_init(&bme);
	ble_midi_encode_set_mtu(&bme, 24);

	ck_assert_int_eq(ble_midi_encode(&bme, midi, sizeof(midi)), 0);

	/* header (1 byte) + timestamp (1 byte) + MIDI message (3 bytes) */
	ck_assert_uint_eq(bme.len, 1 + 1 + sizeof(midi));
	ck_assert_uint_eq(bme.buffer[0] >> 6, 0x02);
	ck_assert_uint_eq(bme.buffer[1] & 0x80, 0x80);
	ck_assert_mem_eq(&bme.buffer[2], midi, sizeof(midi));

} CK_END_TEST

CK_START_TEST(test_ble_midi_encode_multiple) {

	const uint8_t midi1[] = { 0xC0, 0x01 };
	const uint8_t midi2[] = { 0x90, 0x40, 0x7f };
	const uint8_t midi3[] = { 0xF8 };

	struct ble_midi_enc bme;
	ble_midi_encode_init(&bme);
	ble_midi_encode_set_mtu(&bme, 24);

	ck_assert_int_eq(ble_midi_encode(&bme, midi1, sizeof(midi1)), 0);
	ck_assert_int_eq(ble_midi_encode(&bme, midi2, sizeof(midi2)), 0);
	ck_assert_int_eq(ble_midi_encode(&bme, midi3, sizeof(midi3)), 0);

	/* The length of the encoded data should be equal to the sum of the
	 * lengths of the encoded MIDI messages plus the length of the header
	 * (1 byte) and the timestamp (1 byte) bytes. */
	ck_assert_uint_eq(bme.len, 4 + sizeof(midi1) + sizeof(midi2) + sizeof(midi3));

	ck_assert_uint_eq(bme.buffer[0] >> 6, 0x02);

	ck_assert_uint_eq(bme.buffer[1] & 0x80, 0x80);
	ck_assert_mem_eq(&bme.buffer[2], midi1, sizeof(midi1));

	ck_assert_uint_eq(bme.buffer[4] & 0x80, 0x80);
	ck_assert_mem_eq(&bme.buffer[5], midi2, sizeof(midi2));

	ck_assert_uint_eq(bme.buffer[8] & 0x80, 0x80);
	ck_assert_mem_eq(&bme.buffer[9], midi3, sizeof(midi3));

} CK_END_TEST

CK_START_TEST(test_ble_midi_encode_multiple_too_long) {

	const uint8_t midi1[] = { 0x80, 0x40, 0x7f };
	const uint8_t midi2[] = { 0x90, 0x40, 0x7f };

	struct ble_midi_enc bme;
	ble_midi_encode_init(&bme);
	ble_midi_encode_set_mtu(&bme, 8);

	ck_assert_int_eq(ble_midi_encode(&bme, midi1, sizeof(midi1)), 0);
	ck_assert_int_eq(ble_midi_encode(&bme, midi2, sizeof(midi2)), -1);
	ck_assert_uint_eq(errno, EMSGSIZE);

	/* Messages up to the MTU should be encoded properly. */
	ck_assert_uint_eq(bme.len, 2 + sizeof(midi1));

} CK_END_TEST

CK_START_TEST(test_ble_midi_encode_system_exclusive) {

	const uint8_t midi1[] = { 0xF0, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07 };
	const uint8_t midi2[] = { 0xF7 };

	struct ble_midi_enc bme;
	ble_midi_encode_init(&bme);
	ble_midi_encode_set_mtu(&bme, 8);

	ck_assert_int_eq(ble_midi_encode(&bme, midi1, sizeof(midi1)), 1);

	ck_assert_uint_eq(bme.len, 8 /* MTU */);
	ck_assert_uint_eq(bme.buffer[0] >> 6, 0x02);
	ck_assert_uint_eq(bme.buffer[1] & 0x80, 0x80);
	ck_assert_mem_eq(&bme.buffer[2], midi1, 6 /* MTU - 2 */);

	ck_assert_int_eq(ble_midi_encode(&bme, midi1, sizeof(midi1)), 0);
	ck_assert_int_eq(ble_midi_encode(&bme, midi2, sizeof(midi2)), 0);

	/* The continuation of the system exclusive message shall not contain
	 * the timestamp byte after the header, but the end of the exclusive
	 * message shall contain the timestamp byte. */
	ck_assert_uint_eq(bme.len, 1 + (sizeof(midi1) - 6) + 1 + sizeof(midi2));
	ck_assert_uint_eq(bme.buffer[0] >> 6, 0x02);
	ck_assert_mem_eq(&bme.buffer[1], &midi1[6], sizeof(midi1) - 6);
	ck_assert_uint_eq(bme.buffer[3] & 0x80, 0x80);
	ck_assert_mem_eq(&bme.buffer[4], midi2, sizeof(midi2));

} CK_END_TEST

int main(void) {

	Suite *s = suite_create(__FILE__);
	TCase *tc = tcase_create(__FILE__);
	SRunner *sr = srunner_create(s);

	suite_add_tcase(s, tc);

	tcase_add_test(tc, test_ble_midi_decode_init);
	tcase_add_test(tc, test_ble_midi_decode_single);
	tcase_add_test(tc, test_ble_midi_decode_multiple);
	tcase_add_test(tc, test_ble_midi_decode_invalid_header);
	tcase_add_test(tc, test_ble_midi_decode_invalid_status);
	tcase_add_test(tc, test_ble_midi_decode_invalid_interleaved_real_time);
	tcase_add_test(tc, test_ble_midi_decode_single_joined);
	tcase_add_test(tc, test_ble_midi_decode_single_real_time);
	tcase_add_test(tc, test_ble_midi_decode_multiple_real_time);
	tcase_add_test(tc, test_ble_midi_decode_single_system_exclusive);
	tcase_add_test(tc, test_ble_midi_decode_multiple_system_exclusive);
	tcase_add_test(tc, test_ble_midi_decode_multiple_system_exclusive_2);
	tcase_add_test(tc, test_ble_midi_decode_multiple_system_exclusive_3);
	tcase_add_test(tc, test_ble_midi_decode_invalid_system_exclusive);
	tcase_add_test(tc, test_ble_midi_decode_single_running_status);
	tcase_add_test(tc, test_ble_midi_decode_single_running_status_with_real_time);
	tcase_add_test(tc, test_ble_midi_decode_single_running_status_with_common);
	tcase_add_test(tc, test_ble_midi_decode_single_timestamp_overflow);
	tcase_add_test(tc, test_ble_midi_decode_multiple_running_status);

	tcase_add_test(tc, test_ble_midi_encode_no_mtu);
	tcase_add_test(tc, test_ble_midi_encode_single);
	tcase_add_test(tc, test_ble_midi_encode_multiple);
	tcase_add_test(tc, test_ble_midi_encode_multiple_too_long);
	tcase_add_test(tc, test_ble_midi_encode_system_exclusive);

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);
	srunner_free(sr);

	return nf == 0 ? 0 : 1;
}
