/*
 * BlueALSA - msbc.c
 * Copyright (c) 2016-2018 Arkadiusz Bokowy
 *               2017 Juha Kuikka
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "msbc.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "shared/log.h"

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
		esco_h2_header_t *tmp = (esco_h2_header_t *)_data;

		if (tmp->sync == ESCO_H2_SYNCWORD &&
				(bool)(tmp->sn0 & 0x1) == (bool)(tmp->sn0 & 0x2) &&
				(bool)(tmp->sn1 & 0x1) == (bool)(tmp->sn1 & 0x2)) {
			h2 = tmp;
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

	if (msbc->initialized) {
		/* Because there is no sbc_reinit_msbc(), we have to initialize encoder
		 * and decoder once more. In order to prevent memory leaks, we have to
		 * release previously allocated resources. */
		sbc_finish(&msbc->dec_sbc);
		sbc_finish(&msbc->enc_sbc);
	}

	debug("Initializing mSBC encoder/decoder");
	if ((errno = -sbc_init_msbc(&msbc->dec_sbc, 0)) != 0)
		goto fail;
	if ((errno = -sbc_init_msbc(&msbc->enc_sbc, 0)) != 0)
		goto fail;

#if DEBUG
	size_t len;
	if ((len = sbc_get_frame_length(&msbc->dec_sbc)) > MSBC_FRAMELEN) {
		warn("Unexpected mSBC frame size: %zd > %d", len, MSBC_FRAMELEN);
		errno = ENOMEM;
		goto fail;
	}
	if ((len = sbc_get_codesize(&msbc->dec_sbc)) > MSBC_CODESIZE) {
		warn("Unexpected mSBC code size: %zd > %d", len, MSBC_CODESIZE);
		errno = ENOMEM;
		goto fail;
	}
	if ((len = sbc_get_frame_length(&msbc->enc_sbc)) > MSBC_FRAMELEN) {
		warn("Unexpected mSBC frame size: %zd > %d", len, MSBC_FRAMELEN);
		errno = ENOMEM;
		goto fail;
	}
	if ((len = sbc_get_codesize(&msbc->enc_sbc)) > MSBC_CODESIZE) {
		warn("Unexpected mSBC code size: %zd > %d", len, MSBC_CODESIZE);
		errno = ENOMEM;
		goto fail;
	}
#endif

	if (!msbc->initialized) {
		if (ffb_init(&msbc->dec_data, sizeof(esco_msbc_frame_t) * 3) == NULL)
			goto fail;
		if (ffb_init(&msbc->dec_pcm, MSBC_CODESAMPLES * 2) == NULL)
			goto fail;
		if (ffb_init(&msbc->enc_data, sizeof(esco_msbc_frame_t) * 3) == NULL)
			goto fail;
		if (ffb_init(&msbc->enc_pcm, MSBC_CODESAMPLES * 2) == NULL)
			goto fail;
	}

	ffb_rewind(&msbc->dec_data);
	ffb_rewind(&msbc->dec_pcm);
	ffb_rewind(&msbc->enc_data);
	ffb_rewind(&msbc->enc_pcm);
	msbc->enc_frames = 0;

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

	sbc_finish(&msbc->dec_sbc);
	sbc_finish(&msbc->enc_sbc);

	ffb_uint8_free(&msbc->dec_data);
	ffb_int16_free(&msbc->dec_pcm);
	ffb_uint8_free(&msbc->enc_data);
	ffb_int16_free(&msbc->enc_pcm);

}

int msbc_decode(struct esco_msbc *msbc) {

	if (!msbc->initialized)
		return errno = EINVAL, -1;

	uint8_t *input = msbc->dec_data.data;
	size_t input_len = ffb_blen_out(&msbc->dec_data);
	int16_t *output = msbc->dec_pcm.tail;
	size_t output_len = ffb_blen_in(&msbc->dec_pcm);

	for (;;) {

		size_t tmp = input_len;
		esco_h2_header_t *h2 = msbc_find_h2_header(input, &input_len);
		esco_msbc_frame_t *frame = (esco_msbc_frame_t *)h2;
		ssize_t len;

		input += tmp - input_len;
		if (frame == NULL || input_len < sizeof(*frame) || output_len < MSBC_CODESIZE)
			break;

		/* TODO: Check SEQ, implement PLC. */

		if ((len = sbc_decode(&msbc->dec_sbc, frame->payload, sizeof(frame->payload),
						output, output_len, NULL)) > 0) {
			output += MSBC_CODESAMPLES;
			output_len -= MSBC_CODESIZE;
			ffb_seek(&msbc->dec_pcm, MSBC_CODESAMPLES);
		}
		else
			warn("mSBC decoding error: %s", strerror(-len));

		input += sizeof(*frame);
		input_len -= sizeof(*frame);

	}

	/* reshuffle remaining data to the beginning of the buffer */
	ffb_shift(&msbc->dec_data, input - msbc->dec_data.data);

	return 0;
}

int msbc_encode(struct esco_msbc *msbc) {

	if (!msbc->initialized)
		return errno = EINVAL, -1;

	/* pre-generated H2 headers */
	static const uint16_t h2[] = {
		0x0801, 0x3801, 0xc801, 0xf801
	};

	int16_t *input = msbc->enc_pcm.data;
	size_t input_len = ffb_blen_out(&msbc->enc_pcm);
	esco_msbc_frame_t *frame = (esco_msbc_frame_t *)msbc->enc_data.tail;
	size_t output_len = ffb_blen_in(&msbc->enc_data);

	while (input_len >= MSBC_CODESIZE &&
			output_len >= sizeof(*frame)) {

		ssize_t len;

		if ((len = sbc_encode(&msbc->enc_sbc, input, input_len,
						frame->payload, sizeof(frame->payload), NULL)) > 0) {

			frame->header._raw = h2[msbc->enc_frames % 4];
			frame->padding = 0;
			msbc->enc_frames++;

			frame++;
			output_len -= sizeof(*frame);
			ffb_seek(&msbc->enc_data, sizeof(*frame));

		}
		else
			warn("mSBC encoding error: %s", strerror(-len));

		input += MSBC_CODESAMPLES;
		input_len -= MSBC_CODESIZE;

	}

	/* reshuffle remaining data to the beginning of the buffer */
	ffb_shift(&msbc->enc_pcm, input - msbc->enc_pcm.data);

	return 0;
}
