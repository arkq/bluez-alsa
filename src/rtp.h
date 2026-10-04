/*
 * BlueALSA - rtp.h
 * SPDX-FileCopyrightText: 2016-2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BLUEALSA_RTP_H_
#define BLUEALSA_RTP_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <endian.h>
#include <stdbool.h>
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
	uint32_t csrc[];
} __attribute__ ((packed)) rtp_header_t;

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

/**
 * LHDC media payload header. */
typedef struct rtp_lhdc_media_header {
#if __BYTE_ORDER == __BIG_ENDIAN
	uint8_t frame_count:6;
	uint8_t latency:2;
#else
	uint8_t latency:2;
	uint8_t frame_count:6;
#endif
	uint8_t seq_number;
} __attribute__ ((packed)) rtp_lhdc_media_header_t;

/**
 * Initialize RTP headers.
 *
 * @param s The memory area where the RTP headers will be initialized.
 * @param hdr The address where the pointer to the RTP header will be stored.
 * @param phdr The address where the pointer to the RTP payload header will
 *   be stored. This parameter might be NULL.
 * @param phdr_size The size of the RTP payload header.
 * @return This function returns the address of the RTP payload region. */
void * rtp_a2dp_init(
		void * s,
		rtp_header_t ** hdr,
		void ** phdr,
		size_t phdr_size);

/**
 * Get A2DP RTP header payload data.
 *
 * @param hdr The pointer to data with RTP header.
 * @param len The total length of the RTP packet (header + payload).
 * @return On success, this function returns pointer to data just after
 *   the RTP header. On failure, NULL is returned. */
void * rtp_a2dp_get_payload(
		const rtp_header_t * hdr,
		size_t len);

/* Structure for storing local state
 * of the ongoing RTP transmission. */
struct rtp_state {

	/* If true, state was synced with incoming RTP frames. */
	bool synced;

	/* sequence number of RTP frame */
	uint16_t seq_number;

	/* RTP timestamp clock based on PCM sample rate according to the following
	 * formula: RTP_ts = PCM_frames / PCM_samplerate * RTP_clockrate */
	unsigned int ts_pcm_frames;
	unsigned int ts_pcm_samplerate;
	unsigned int ts_rtp_clockrate;
	uint32_t ts_offset;

};

/**
 * Initialize RTP local state.
 *
 * @param rtp Address of the RTP state structure.
 * @param pcm_samplerate PCM audio sample rate used for driving RTP clock.
 * @param rtp_clockrate Desired clock rate of the RTP clock. */
void rtp_state_init(
		struct rtp_state * rtp,
		unsigned int pcm_samplerate,
		unsigned int rtp_clockrate);

/**
 * Generate new RTP frame.
 *
 * @param rtp The RTP state structure.
 * @param hdr The RTP header which will be updated. */
void rtp_state_new_frame(
		struct rtp_state * rtp,
		rtp_header_t * hdr);

/**
 * Synchronize local RTP state with RTP stream.
 *
 * @param rtp The RTP state structure.
 * @param hdr The RTP header of received RTP frame.
 * @param missing_rtp_frames If not NULL, the number of missing RTP frames will
 *   be stored at the given address.
 * @param missing_pcm_frames If not NULL, the number of missing PCM frames will
 *   be stored at the given address. */
void rtp_state_sync_stream(
		struct rtp_state * rtp,
		const rtp_header_t * hdr,
		int * missing_rtp_frames,
		int * missing_pcm_frames);

/**
 * Update local RTP state.
 *
 * @param rtp The RTP state structure.
 * @param pcm_frames The number of transferred PCM frames. */
void rtp_state_update(
		struct rtp_state * rtp,
		unsigned int pcm_frames);

#endif
