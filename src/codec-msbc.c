/*
 * BlueALSA - codec-msbc.c
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
 *               2017 Juha Kuikka
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "codec-msbc.h"

#include <endian.h>
#include <errno.h>
#include <stdbool.h>

#include "codec-sbc.h"
#include "shared/log.h"

/* Code protected 2-bit sequence numbers (SN0 and SN1) used
 * by the msbc_encode() function. */
static const uint8_t sn[][2] = {
	{ 0, 0 }, { 3, 0 }, { 0, 3 }, { 3, 3 }
};

/**
 * Find H2 synchronization header within eSCO transparent data.
 *
 * @param data Memory area with the eSCO transparent data.
 * @param len Address from where the length of the eSCO transparent data
 *   is read. Upon exit, the remaining length of the eSCO data will be
 *   stored in this variable (received length minus scanned length).
 * @return On success this function returns the first occurrence of the H2
 *   synchronization header. Otherwise, it returns NULL. */
static esco_h2_header_t *msbc_find_h2_header(const void *data, size_t *len) {

	esco_h2_header_t *h2 = NULL;
	const uint8_t *_data = data;
	size_t _len = *len;

	while (_len >= sizeof(esco_h2_header_t)) {
		esco_h2_header_t tmp = le16toh(*(esco_h2_header_t *)_data);

		if (ESCO_H2_GET_SYNCWORD(tmp) == ESCO_H2_SYNCWORD &&
				(ESCO_H2_GET_SN0(tmp) >> 1) == (ESCO_H2_GET_SN0(tmp) & 1) &&
				(ESCO_H2_GET_SN1(tmp) >> 1) == (ESCO_H2_GET_SN1(tmp) & 1)) {
			h2 = (esco_h2_header_t *)_data;
			goto final;
		}

		_data += 1;
		_len--;
	}

final:
	*len = _len;
	return h2;
}

int msbc_init(struct esco_msbc *msbc) {

	int err;

	if (!msbc->initialized) {
		debug("Initializing mSBC codec");
		if ((errno = -sbc_init_msbc(&msbc->sbc, 0)) != 0)
			goto fail;
		if (ffb_init_uint8_t(&msbc->data, sizeof(esco_msbc_frame_t) * 3) == -1)
			goto fail;
		if (ffb_init_int16_t(&msbc->pcm, MSBC_CODESAMPLES * 2) == -1)
			goto fail;
	}

	if ((errno = -sbc_reinit_msbc(&msbc->sbc, 0)) != 0)
		return -1;

#if DEBUG
	size_t len;
	if ((len = sbc_get_frame_length(&msbc->sbc)) > MSBC_FRAMELEN) {
		warn("Unexpected mSBC frame size: %zd > %d", len, MSBC_FRAMELEN);
		errno = ENOMEM;
		goto fail;
	}
	if ((len = sbc_get_codesize(&msbc->sbc)) > MSBC_CODESIZE) {
		warn("Unexpected mSBC code size: %zd > %d", len, MSBC_CODESIZE);
		errno = ENOMEM;
		goto fail;
	}
#endif

	ffb_rewind(&msbc->data);
	ffb_rewind(&msbc->pcm);

	msbc->seq_initialized = false;
	msbc->seq_number = 0;
	msbc->frames = 0;

	msbc->initialized = true;
	return 0;

fail:
	err = errno;
	msbc_finish(msbc);
	errno = err;
	return -1;
}

void msbc_finish(struct esco_msbc *msbc) {

	if (msbc == NULL)
		return;

	sbc_finish(&msbc->sbc);

	ffb_free(&msbc->data);
	ffb_free(&msbc->pcm);

}

/**
 * Find and decode single eSCO mSBC frame. */
int msbc_decode(struct esco_msbc *msbc) {

	if (!msbc->initialized)
		return errno = EINVAL, -1;

	const uint8_t *input = msbc->data.data;
	size_t input_len = ffb_blen_out(&msbc->data);
	int16_t *output = msbc->pcm.tail;
	size_t output_len = ffb_blen_in(&msbc->pcm);
	int rv = 0;

	const size_t tmp = input_len;
	const esco_h2_header_t *_h2 = msbc_find_h2_header(input, &input_len);
	const esco_msbc_frame_t *frame = (esco_msbc_frame_t *)_h2;
	input += tmp - input_len;

	/* Skip decoding if there is not enough input data or the output
	 * buffer is not big enough to hold decoded PCM samples.*/
	if (input_len < sizeof(*frame) ||
			output_len < MSBC_CODESIZE)
		goto final;

	const uint16_t h2 = le16toh(*_h2);
	uint8_t _seq = (ESCO_H2_GET_SN1(h2) & 2) | (ESCO_H2_GET_SN0(h2) & 1);
	if (!msbc->seq_initialized) {
		msbc->seq_initialized = true;
		msbc->seq_number = _seq;
	}
	else if (_seq != ++msbc->seq_number) {
		warn("Missing mSBC packet: %u != %u", _seq, msbc->seq_number);
		msbc->seq_number = _seq;
		/* TODO: Implement PLC. */
	}

	ssize_t len;
	if ((len = sbc_decode(&msbc->sbc, frame->payload, sizeof(frame->payload),
					output, output_len, NULL)) < 0) {
		errno = -len, rv = -1;
		input += 1;
		goto final;
	}

	ffb_seek(&msbc->pcm, MSBC_CODESAMPLES);
	input += sizeof(*frame);
	rv = 1;

final:
	/* Reshuffle remaining data to the beginning of the buffer. */
	ffb_shift(&msbc->data, input - (uint8_t *)msbc->data.data);
	return rv;
}

/**
 * Encode single eSCO mSBC frame. */
int msbc_encode(struct esco_msbc *msbc) {

	if (!msbc->initialized)
		return errno = EINVAL, -1;

	const int16_t *input = msbc->pcm.data;
	const size_t input_len = ffb_blen_out(&msbc->pcm);
	esco_msbc_frame_t *frame = (esco_msbc_frame_t *)msbc->data.tail;
	size_t output_len = ffb_blen_in(&msbc->data);

	/* Skip encoding if there is not enough PCM samples or the output
	 * buffer is not big enough to hold whole eSCO mSBC frame.*/
	if (input_len < MSBC_CODESIZE ||
			output_len < sizeof(*frame))
		return 0;

	ssize_t len;
	if ((len = sbc_encode(&msbc->sbc, input, input_len,
					frame->payload, sizeof(frame->payload), NULL)) < 0)
		return errno = -len, -1;

	const uint8_t n = msbc->seq_number++;
	frame->header = htole16(ESCO_H2_PACK(sn[n][0], sn[n][1]));
	frame->padding = 0;

	ffb_seek(&msbc->data, sizeof(*frame));
	msbc->frames++;

	/* Reshuffle remaining PCM data to the beginning of the buffer. */
	ffb_shift(&msbc->pcm, input + MSBC_CODESAMPLES - (int16_t *)msbc->pcm.data);

	return 1;
}
