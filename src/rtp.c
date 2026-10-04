/*
 * BlueALSA - rtp.c
 * SPDX-FileCopyrightText: 2016-2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#include "rtp.h"

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <endian.h>
#include <errno.h>
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

void * rtp_a2dp_init(
		void * s, rtp_header_t ** hdr, void ** phdr, size_t phdr_size) {

	rtp_header_t * header = *hdr = s;
	memset(header, 0, sizeof(*header) + phdr_size);
	header->paytype = 96;
	header->version = 2;

	uint8_t * data = (uint8_t *)&header->csrc[header->cc];

	if (phdr != NULL)
		*phdr = data;

	return data + phdr_size;
}

void * rtp_a2dp_get_payload(
		const rtp_header_t * hdr,
		size_t len) {

	if (len < sizeof(*hdr))
		return errno = EINVAL, NULL;

#if ENABLE_PAYLOADCHECK
	if (hdr->paytype < 96) {
		warn("Unsupported RTP payload type: %u", hdr->paytype);
		return errno = EMEDIUMTYPE, NULL;
	}
#endif

	if (len < sizeof(*hdr) + hdr->cc * sizeof(uint32_t))
		return errno = EMSGSIZE, NULL;

	return (void *)&hdr->csrc[hdr->cc];
}

void rtp_state_init(
		struct rtp_state * rtp,
		unsigned int pcm_samplerate,
		unsigned int rtp_clockrate) {

	rtp->synced = false;

	rtp->seq_number = rand();

	rtp->ts_pcm_frames = 0;
	rtp->ts_pcm_samplerate = pcm_samplerate;
	rtp->ts_rtp_clockrate = rtp_clockrate;
	rtp->ts_offset = rand();

}

void rtp_state_new_frame(
		struct rtp_state *rtp,
		rtp_header_t *hdr) {

	uint32_t timestamp = rtp_convert_clock_rate(rtp->ts_pcm_frames,
			rtp->ts_pcm_samplerate, rtp->ts_rtp_clockrate) + rtp->ts_offset;

	hdr->seq_number = htobe16(++rtp->seq_number);
	hdr->timestamp = htobe32(timestamp);

}

void rtp_state_sync_stream(
		struct rtp_state * rtp,
		const rtp_header_t * hdr,
		int * missing_rtp_frames,
		int * missing_pcm_frames) {

	uint16_t hdr_seq_number = be16toh(hdr->seq_number);
	uint32_t hdr_timestamp = be32toh(hdr->timestamp);

	if (!rtp->synced) {
		rtp->seq_number = hdr_seq_number;
		rtp->ts_offset = hdr_timestamp;
		rtp->synced = true;
		return;
	}

	/* Increment local RTP sequence number. */
	uint16_t expect_seq_number = ++rtp->seq_number;

	/* Check for missing RTP frames. */
	if (missing_rtp_frames != NULL) {
		if ((*missing_rtp_frames = hdr_seq_number - expect_seq_number) != 0) {
			warn("Missing RTP packets [%u != %u]: %d",
					hdr_seq_number, expect_seq_number, *missing_rtp_frames);
			rtp->seq_number = hdr_seq_number;
		}
	}

	/* Check for missing PCM frames. */
	if (missing_pcm_frames != NULL) {

		/* Calculate expected PCM frames based on local timestamp. */
		const uint32_t timestamp = hdr_timestamp - rtp->ts_offset;
		unsigned int expect_pcm_frames = rtp_convert_clock_rate(timestamp,
				rtp->ts_rtp_clockrate, rtp->ts_pcm_samplerate);

		if ((*missing_pcm_frames = expect_pcm_frames - rtp->ts_pcm_frames) != 0) {
			debug("Missing PCM frames [%u]: %d", hdr_timestamp, *missing_pcm_frames);
			rtp->ts_pcm_frames = expect_pcm_frames;
		}

	}

}

void rtp_state_update(
		struct rtp_state * rtp,
		unsigned int pcm_frames) {

	rtp->ts_pcm_frames += pcm_frames;

}
