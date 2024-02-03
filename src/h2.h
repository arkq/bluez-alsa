/*
 * BlueALSA - h2.h
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_H2_H_
#define BLUEALSA_H2_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <endian.h>
#include <stddef.h>
#include <stdint.h>

#define H2_SYNCWORD         0x801
#define H2_GET_SYNCWORD(h2) ((h2) & 0xFFF)
#define H2_GET_SN0(h2)      (((h2) >> 12) & 0x3)
#define H2_GET_SN1(h2)      (((h2) >> 14) & 0x3)

typedef uint16_t h2_header_t;

/**
 * Pack H2 synchronization header.
 *
 * @param seq Sequence number, which has to be in range from 0 to 3.
 * @return Packed H2 synchronization header. */
static inline h2_header_t h2_header_pack(uint8_t seq) {
	/* code protected 2-bit sequence numbers (SN0 and SN1) */
	static const uint8_t sn[][2] = {
		{ 0, 0 }, { 3, 0 }, { 0, 3 }, { 3, 3 } };
	return htole16(H2_SYNCWORD | sn[seq][0] << 12 | sn[seq][1] << 14);
}

/**
 * Unpack H2 synchronization header.
 *
 * @param h2 Packed H2 synchronization header.
 * @return Unpacked sequence number. */
static inline uint8_t h2_header_unpack(h2_header_t h2) {
	const uint16_t v = le16toh(h2);
	return (H2_GET_SN1(v) & 2) | (H2_GET_SN0(v) & 1);
}

void *h2_header_find(const void *data, size_t *len);

#endif
