/*
 * BlueALSA - a2dp-rtp.h
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_A2DPRTP_H_
#define BLUEALSA_A2DPRTP_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <endian.h>
#include <stddef.h>
#include <stdint.h>

typedef struct rtp_header {
#if __BYTE_ORDER == __LITTLE_ENDIAN
	uint16_t cc:4;
	uint16_t extbit:1;
	uint16_t padbit:1;
	uint16_t version:2;
	uint16_t paytype:7;
	uint16_t markbit:1;
#else
	uint16_t version:2;
	uint16_t padbit:1;
	uint16_t extbit:1;
	uint16_t cc:4;
	uint16_t markbit:1;
	uint16_t paytype:7;
#endif
	uint16_t seq_number;
	uint32_t timestamp;
	uint32_t ssrc;
	uint32_t csrc[16];
} __attribute__ ((packed)) rtp_header_t;

/**
 * The length of the RTP header assuming that the `cc` field is set to zero. */
#define RTP_HEADER_LEN (sizeof(rtp_header_t) - sizeof(((rtp_header_t *)0)->csrc))

/**
 * Media payload header. */
typedef struct rtp_media_header {
#if __BYTE_ORDER == __LITTLE_ENDIAN
	uint8_t frame_count:4;
	uint8_t rfa:1;
	uint8_t last_fragment:1;
	uint8_t first_fragment:1;
	uint8_t fragmented:1;
#else
	uint8_t fragmented:1;
	uint8_t first_fragment:1;
	uint8_t last_fragment:1;
	uint8_t rfa:1;
	uint8_t frame_count:4;
#endif
} __attribute__ ((packed)) rtp_media_header_t;

/**
 * MPEG audio payload header.
 * See: https://tools.ietf.org/html/rfc2250 */
typedef struct rtp_mpeg_audio_header {
	uint16_t rfa;
	uint16_t offset;
} __attribute__ ((packed)) rtp_mpeg_audio_header_t;

void *a2dp_rtp_init(void *s, rtp_header_t **hdr, void **phdr, size_t phdr_size);
void *a2dp_rtp_payload(const rtp_header_t *hdr, uint16_t *seq_number);

#endif
