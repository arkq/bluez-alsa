/*
 * BlueALSA - codec-lc3-swb.c
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "codec-lc3-swb.h"

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdbool.h>
#include <stdint.h>

#include <lc3.h>

#include "h2.h"
#include "shared/log.h"

/**
 * Initialize LC3-SWB codec structure.
 *
 * This function is idempotent, so it can be called multiple times in order
 * to reinitialize the codec structure.
 *
 * @param lc3_swb Codec structure which shall be initialized. */
void lc3_swb_init(struct esco_lc3_swb *lc3_swb) {

	lc3_swb->encoder = lc3_setup_encoder(7500, 32000, 0, &lc3_swb->mem_encoder);
	lc3_swb->decoder = lc3_setup_decoder(7500, 32000, 0, &lc3_swb->mem_decoder);

	ffb_init_from_array(&lc3_swb->data, lc3_swb->buffer_data);
	ffb_init_from_array(&lc3_swb->pcm, lc3_swb->buffer_pcm);

	lc3_swb->seq_initialized = false;
	lc3_swb->seq_number = 0;
	lc3_swb->frames = 0;

}

/**
 * Get LC3-SWB delay in number of samples. */
ssize_t lc3_swb_get_delay(struct esco_lc3_swb *lc3_swb) {
	(void)lc3_swb;
	return lc3_delay_samples(7500, 32000);
}

/**
 * Encode single eSCO LC3-SWB frame. */
ssize_t lc3_swb_encode(struct esco_lc3_swb *lc3_swb) {

	const int16_t *input = lc3_swb->pcm.data;
	const size_t input_samples = ffb_len_out(&lc3_swb->pcm);
	h2_lc3_swb_frame_t *frame = lc3_swb->data.tail;
	size_t output_len = ffb_blen_in(&lc3_swb->data);

	/* Skip encoding if there is not enough PCM samples or the output
	 * buffer is not big enough to hold whole eSCO LC3-SWB frame.*/
	if (input_samples < LC3_SWB_CODESAMPLES ||
			output_len < sizeof(*frame))
		return 0;

	lc3_encode(lc3_swb->encoder, LC3_PCM_FORMAT_S16, input, 1,
			sizeof(frame->payload), frame->payload);
	frame->header = h2_header_pack(++lc3_swb->seq_number);

	ffb_seek(&lc3_swb->data, sizeof(*frame));
	lc3_swb->frames++;

	/* Reshuffle remaining PCM data to the beginning of the buffer. */
	ffb_shift(&lc3_swb->pcm, LC3_SWB_CODESAMPLES);

	return sizeof(*frame);
}

/**
 * Find and decode single eSCO LC3-SWB frame. */
ssize_t lc3_swb_decode(struct esco_lc3_swb *lc3_swb) {

	const uint8_t *input = lc3_swb->data.data;
	size_t input_len = ffb_blen_out(&lc3_swb->data);
	size_t output_len = ffb_blen_in(&lc3_swb->pcm);
	ssize_t rv = 0;

	const size_t tmp = input_len;
	const h2_lc3_swb_frame_t *frame = h2_header_find(input, &input_len);
	input += tmp - input_len;

	/* Skip decoding if there is not enough input data or the output
	 * buffer is not big enough to hold decoded PCM samples and PCM
	 * samples reconstructed with PLC (up to 3 LC3-SWB frames). */
	if (input_len < sizeof(*frame) ||
			output_len < LC3_SWB_CODESIZE * (1 + 3))
		goto final;

	const uint8_t h2_seq = h2_header_unpack(frame->header);

	if (!lc3_swb->seq_initialized) {
		lc3_swb->seq_initialized = true;
		lc3_swb->seq_number = h2_seq;
	}
	else if (h2_seq != ++lc3_swb->seq_number) {

		/* In case of missing LC3-SWB frames (we can detect up to 3 consecutive
		 * missing frames) use PLC for PCM samples reconstruction. */

		uint8_t missing = (h2_seq + 4 - lc3_swb->seq_number) % 4;
		warn("Missing LC3-SWB packets (%u != %u): %u",
				h2_seq, lc3_swb->seq_number, missing);

		lc3_swb->seq_number = h2_seq;

		while (missing--) {
			lc3_decode(lc3_swb->decoder, NULL, 0,
					LC3_PCM_FORMAT_S16, lc3_swb->pcm.tail, 1);
			ffb_seek(&lc3_swb->pcm, LC3_SWB_CODESAMPLES);
			rv += LC3_SWB_CODESAMPLES;
		}

	}

	/* Decode LC3-SWB frame. In case of bitstream corruption, this function
	 * internally uses PLC for PCM samples reconstruction. */
	if (lc3_decode(lc3_swb->decoder, frame->payload, sizeof(frame->payload),
				LC3_PCM_FORMAT_S16, lc3_swb->pcm.tail, 1) != 0)
		warn("Couldn't decode LC3-SWB frame: %s", "Bitstream corrupted");

	ffb_seek(&lc3_swb->pcm, LC3_SWB_CODESAMPLES);
	rv += LC3_SWB_CODESAMPLES;
	input += sizeof(*frame);

final:
	/* Reshuffle remaining data to the beginning of the buffer. */
	ffb_shift(&lc3_swb->data, input - (uint8_t *)lc3_swb->data.data);
	return rv;
}
