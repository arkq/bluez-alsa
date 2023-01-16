/*
 * BlueALSA - rtp.c
 * Copyright (c) 2016-2022 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "rtp.h"
/* IWYU pragma: no_include "config.h" */

#include <endian.h>
#include <stdlib.h>
#include <string.h>

#include "shared/defs.h"
#include "shared/log.h"

/**
 * Convert clock rate. */
static unsigned int rtp_convert_clock_rate(
		unsigned int ticks,
		unsigned int rate_from,
		unsigned int rate_to) {
	/* NOTE: In all our cases clockrates/samplerates are even numbers. In order
	 *       to round-up converted value we are going to use halve rate_from in
	 *       our arithmetic. */
	return DIV_ROUND_UP((uint64_t)ticks * rate_to / (rate_from / 2), 2);
}

/**
 * Initialize RTP headers.
 *
 * @param s The memory area where the RTP headers will be initialized.
 * @param hdr The address where the pointer to the RTP header will be stored.
 * @param phdr The address where the pointer to the RTP payload header will
 *   be stored. This parameter might be NULL.
 * @param phdr_size The size of the RTP payload header.
 * @return This function returns the address of the RTP payload region. */
void *rtp_a2dp_init(void *s, rtp_header_t **hdr, void **phdr, size_t phdr_size) {

	rtp_header_t *header = *hdr = (rtp_header_t *)s;
	memset(header, 0, RTP_HEADER_LEN + phdr_size);
	header->paytype = 96;
	header->version = 2;

	uint8_t *data = (uint8_t *)&header->csrc[header->cc];

	if (phdr != NULL)
		*phdr = data;

	return data + phdr_size;
}

/**
 * Get A2DP RTP header payload data.
 *
 * @param hdr The pointer to data with RTP header.
 * @return On success, this function returns pointer to data just after
 *   the RTP header - RTP header payload. On failure, NULL is returned. */
void *rtp_a2dp_get_payload(const rtp_header_t *hdr) {

#if ENABLE_PAYLOADCHECK
	if (hdr->paytype < 96) {
		warn("Unsupported RTP payload type: %u", hdr->paytype);
		return NULL;
	}
#endif

	return (void *)&hdr->csrc[hdr->cc];
}

/**
 * Initialize RTP local state.
 *
 * @param rtp Address of the RTP state structure.
 * @param pcm_samplerate PCM audio sample rate used for driving RTP clock.
 * @param rtp_clockrate Desired clock rate of the RTP clock. */
void rtp_state_init(
		struct rtp_state *rtp,
		unsigned int pcm_samplerate,
		unsigned int rtp_clockrate) {

	rtp->synced = false;

	rtp->seq_number = rand();

	rtp->ts_pcm_frames = 0;
	rtp->ts_pcm_samplerate = pcm_samplerate;
	rtp->ts_rtp_clockrate = rtp_clockrate;
	rtp->ts_offset = rand();

}

/**
 * Generate new RTP frame.
 *
 * @param rtp The RTP state structure.
 * @param hdr The RTP header which will be updated. */
void rtp_state_new_frame(
		struct rtp_state *rtp,
		rtp_header_t *hdr) {

	uint32_t timestamp = rtp_convert_clock_rate(rtp->ts_pcm_frames,
			rtp->ts_pcm_samplerate, rtp->ts_rtp_clockrate) + rtp->ts_offset;

	hdr->seq_number = htobe16(++rtp->seq_number);
	hdr->timestamp = htobe32(timestamp);

}

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
		struct rtp_state *rtp,
		const rtp_header_t *hdr,
		int *missing_rtp_frames,
		int *missing_pcm_frames) {

	uint16_t hdr_seq_number = be16toh(hdr->seq_number);
	uint32_t hdr_timestamp = be32toh(hdr->timestamp);

	if (!rtp->synced) {
		rtp->seq_number = hdr_seq_number;
		rtp->ts_offset = hdr_timestamp;
		rtp->synced = true;
		return;
	}

	/* increment local RTP sequence number */
	uint16_t expect_seq_number = ++rtp->seq_number;

	/* check for missing RTP frames */
	if (missing_rtp_frames != NULL) {
		if ((*missing_rtp_frames = hdr_seq_number - expect_seq_number) != 0) {
			warn("Missing RTP packets [%u != %u]: %d",
					hdr_seq_number, expect_seq_number, *missing_rtp_frames);
			rtp->seq_number = hdr_seq_number;
		}
	}

	/* check for missing PCM frames */
	if (missing_pcm_frames != NULL) {

		/* calculate expected PCM frames based on local timestamp */
		const uint32_t timestamp = hdr_timestamp - rtp->ts_offset;
		unsigned int expect_pcm_frames = rtp_convert_clock_rate(timestamp,
				rtp->ts_rtp_clockrate, rtp->ts_pcm_samplerate);

		if ((*missing_pcm_frames = expect_pcm_frames - rtp->ts_pcm_frames) != 0) {
			debug("Missing PCM frames [%u]: %d", hdr_timestamp, *missing_pcm_frames);
			rtp->ts_pcm_frames = expect_pcm_frames;
		}

	}

}

/**
 * Update local RTP state.
 *
 * @param rtp The RTP state structure.
 * @param pcm_frames The number of transferred PCM frames. */
void rtp_state_update(
		struct rtp_state *rtp,
		unsigned int pcm_frames) {

	rtp->ts_pcm_frames += pcm_frames;

}
