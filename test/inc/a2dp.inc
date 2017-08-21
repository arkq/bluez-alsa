/*
 * a2dp.inc
 * vim: ft=c
 *
 * Copyright (c) 2016-2017 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sbc/sbc.h>
#include "../src/a2dp-codecs.h"
#include "../src/a2dp-rtp.h"

/**
 * Encode audio buffer using SBC codec.
 *
 * This function encodes data from the buffer using SBC codec and writes it to
 * the file referred to by the file descriptor. Encoded data is encapsulated
 * with the standard RTP header and the appropriate SBC payload header. */
int a2dp_write_sbc(int fd, const a2dp_sbc_t *config, const void *buffer, size_t size) {

	sbc_t sbc;
	int ret = 0;

	if ((ret = sbc_init_a2dp(&sbc, 0, config, sizeof(*config))) != 0)
		return ret;

	const size_t sbc_codesize = sbc_get_codesize(&sbc);
	const size_t sbc_frame_len = sbc_get_frame_length(&sbc);
	const unsigned sbc_frame_duration = sbc_get_frame_duration(&sbc);

	rtp_header_t *rtp_header;
	rtp_payload_sbc_t *rtp_payload;
	uint16_t seq_number = 0;
	uint32_t timestamp = 0;

	const size_t wbuffer_size = 500;
	uint8_t *wbuffer = malloc(wbuffer_size);

	rtp_header = (rtp_header_t *)wbuffer;
	memset(rtp_header, 0, sizeof(*rtp_header));
	rtp_payload = (rtp_payload_sbc_t *)&rtp_header->csrc[rtp_header->cc];
	memset(rtp_payload, 0, sizeof(*rtp_payload));
	rtp_header->version = 2;
	rtp_header->paytype = 96;

	while (size >= sbc_codesize) {

		uint8_t *output = (uint8_t *)(rtp_payload + 1);
		size_t output_len = wbuffer_size - (output - wbuffer);
		size_t frames = 0;

		while (size >= sbc_codesize && output_len >= sbc_frame_len) {

			ssize_t len;
			ssize_t encoded;

			len = sbc_encode(&sbc, buffer, size, output, output_len, &encoded);

			buffer += len;
			size -= len;
			output += encoded;
			output_len -= encoded;
			frames++;

		}

		rtp_header->seq_number = htons(++seq_number);
		rtp_header->timestamp = htonl(timestamp);
		rtp_payload->frame_count = frames;

		if (write(fd, wbuffer, output - wbuffer) == -1) {
			ret = -errno;
			goto fail;
		}

		timestamp += sbc_frame_duration * frames;
	}

fail:
	sbc_finish(&sbc);
	return ret;
}
