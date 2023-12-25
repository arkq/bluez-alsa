/*
 * BlueALSA - codec-msbc.c
 * Copyright (c) 2016-2023 Arkadiusz Bokowy
 * Copyright (c) 2017 Juha Kuikka
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "codec-msbc.h"

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <endian.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <spandsp.h>

#include "codec-sbc.h"
#include "shared/log.h"

/**
 * Use PLC in case of SBC decoding error.
 *
 * If defined to 1, in case of SBC frame decoding error the msbc_decode()
 * function will not return error code, but will use PLC to conceal missing
 * PCM samples. Such behavior should ensure that a PCM client will receive
 * correct number of PCM samples - matching sampling frequency. */
#define MSBC_DECODE_ERROR_PLC 1

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
 * @return On success this function returns address of the first occurrence
 *   of the H2 synchronization header. Otherwise, it returns NULL. */
static void *msbc_find_h2_header(const void *data, size_t *len) {

	const uint8_t *_data = data;
	size_t _len = *len;
	void *ptr = NULL;

	while (_len >= sizeof(esco_h2_header_t)) {

		esco_h2_header_t h2;
		memcpy(&h2, _data, sizeof(h2));
		h2 = le16toh(h2);

		if (ESCO_H2_GET_SYNCWORD(h2) == ESCO_H2_SYNCWORD &&
				(ESCO_H2_GET_SN0(h2) >> 1) == (ESCO_H2_GET_SN0(h2) & 1) &&
				(ESCO_H2_GET_SN1(h2) >> 1) == (ESCO_H2_GET_SN1(h2) & 1)) {
			ptr = (void *)_data;
			goto final;
		}

		_data += 1;
		_len--;
	}

final:
	*len = _len;
	return ptr;
}

/**
 * Initialize mSBC codec structure.
 *
 * This function is idempotent, so it can be called multiple times in order
 * to reinitialize the codec structure.
 *
 * @param msbc Codec structure which shall be initialized.
 * @return This function returns 0 on success or a negative error value
 *   in case of initialization failure. */
int msbc_init(struct esco_msbc *msbc) {

	if (!msbc->initialized) {
		debug("Initializing mSBC codec");
		if (ffb_init_uint8_t(&msbc->data, sizeof(esco_msbc_frame_t) * 3) == -1)
			goto fail_init;
		/* Allocate buffer for 1 decoded frame, optional 3 PLC frames and
		 * some extra frames to account for async PCM samples reading. */
		if (ffb_init_int16_t(&msbc->pcm, MSBC_CODESAMPLES * 6) == -1)
			goto fail_init;
		if ((errno = -sbc_init_msbc(&msbc->sbc, 0)) != 0)
			goto fail_init;
	}
	else {
		debug("Re-initializing mSBC codec");
		if ((errno = -sbc_reinit_msbc(&msbc->sbc, 0)) != 0)
			goto fail_init;
	}

	/* ensure libsbc uses little-endian PCM on all architectures */
	msbc->sbc.endian = SBC_LE;

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

	if (msbc->plc != NULL)
		plc_init(msbc->plc);
	else if (!(msbc->plc = plc_init(NULL)))
		goto fail;

	msbc->initialized = true;
	return 0;

fail:
	sbc_finish(&msbc->sbc);
fail_init:
	ffb_free(&msbc->data);
	ffb_free(&msbc->pcm);
	return -errno;
}

void msbc_finish(struct esco_msbc *msbc) {

	if (msbc == NULL)
		return;

	sbc_finish(&msbc->sbc);

	ffb_free(&msbc->data);
	ffb_free(&msbc->pcm);

	plc_free(msbc->plc);
	msbc->plc = NULL;

}

/**
 * Find and decode single eSCO mSBC frame. */
ssize_t msbc_decode(struct esco_msbc *msbc) {

	if (!msbc->initialized)
		return -EINVAL;

	const uint8_t *input = msbc->data.data;
	size_t input_len = ffb_blen_out(&msbc->data);
	size_t output_len = ffb_blen_in(&msbc->pcm);
	ssize_t rv = 0;

	const size_t tmp = input_len;
	const esco_msbc_frame_t *frame = msbc_find_h2_header(input, &input_len);
	input += tmp - input_len;

	/* Skip decoding if there is not enough input data or the output
	 * buffer is not big enough to hold decoded PCM samples and PCM
	 * samples reconstructed with PLC (up to 3 mSBC frames). */
	if (input_len < sizeof(*frame) ||
			output_len < MSBC_CODESIZE * (1 + 3))
		goto final;

	esco_h2_header_t h2;
	memcpy(&h2, frame, sizeof(h2));
	h2 = le16toh(h2);

	uint8_t _seq = (ESCO_H2_GET_SN1(h2) & 2) | (ESCO_H2_GET_SN0(h2) & 1);
	if (!msbc->seq_initialized) {
		msbc->seq_initialized = true;
		msbc->seq_number = _seq;
	}
	else if (_seq != ++msbc->seq_number) {

		/* In case of missing mSBC frames (we can detect up to 3 consecutive
		 * missing frames) use PLC for PCM samples reconstruction. */

		uint8_t missing = (_seq + ESCO_H2_SN_MAX - msbc->seq_number) % ESCO_H2_SN_MAX;
		warn("Missing mSBC packets (%u != %u): %u", _seq, msbc->seq_number, missing);

		msbc->seq_number = _seq;

		plc_fillin(msbc->plc, msbc->pcm.tail, missing * MSBC_CODESAMPLES);
		ffb_seek(&msbc->pcm, missing * MSBC_CODESAMPLES);
		rv += missing * MSBC_CODESAMPLES;

	}

	ssize_t len;
	if ((len = sbc_decode(&msbc->sbc, frame->payload, sizeof(frame->payload),
					msbc->pcm.tail, output_len, NULL)) < 0) {

		/* Move forward one byte to avoid getting stuck in
		 * decoding the same mSBC packet all over again. */
		input += 1;

#if MSBC_DECODE_ERROR_PLC

		warn("Couldn't decode mSBC frame: %s", sbc_strerror(len));
		plc_fillin(msbc->plc, msbc->pcm.tail, MSBC_CODESAMPLES);
		ffb_seek(&msbc->pcm, MSBC_CODESAMPLES);
		rv += MSBC_CODESAMPLES;

#else
		rv = len;
#endif

		goto final;
	}

	/* record PCM history and blend new data after PLC */
	plc_rx(msbc->plc, msbc->pcm.tail, MSBC_CODESAMPLES);

	ffb_seek(&msbc->pcm, MSBC_CODESAMPLES);
	input += sizeof(*frame);
	rv += MSBC_CODESAMPLES;

final:
	/* Reshuffle remaining data to the beginning of the buffer. */
	ffb_shift(&msbc->data, input - (uint8_t *)msbc->data.data);
	return rv;
}

/**
 * Encode single eSCO mSBC frame. */
ssize_t msbc_encode(struct esco_msbc *msbc) {

	if (!msbc->initialized)
		return -EINVAL;

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
		return len;

	const uint8_t n = msbc->seq_number++;
	frame->header = htole16(ESCO_H2_PACK(sn[n][0], sn[n][1]));
	frame->padding = 0;

	ffb_seek(&msbc->data, sizeof(*frame));
	msbc->frames++;

	/* Reshuffle remaining PCM data to the beginning of the buffer. */
	ffb_shift(&msbc->pcm, input + MSBC_CODESAMPLES - (int16_t *)msbc->pcm.data);

	return sizeof(*frame);
}

/**
 * Get string representation of the mSBC encode/decode error.
 *
 * @param err The encode/decode error code.
 * @return Human-readable string. */
const char *msbc_strerror(int err) {
	return sbc_strerror(err);
}
