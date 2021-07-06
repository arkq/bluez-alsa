/*
 * BlueALSA - a2dp-rtp.c
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "a2dp-rtp.h"

#include <stdlib.h>
#include <string.h>

#include "shared/log.h"

/**
 * Initialize RTP headers.
 *
 * @param s The memory area where the RTP headers will be initialized.
 * @param hdr The address where the pointer to the RTP header will be stored.
 * @param phdr The address where the pointer to the RTP payload header will
 *   be stored. This parameter might be NULL.
 * @param phdr_size The size of the RTP payload header.
 * @return This function returns the address of the RTP payload region. */
void *a2dp_rtp_init(void *s, rtp_header_t **hdr, void **phdr, size_t phdr_size) {

	rtp_header_t *header = *hdr = (rtp_header_t *)s;
	memset(header, 0, RTP_HEADER_LEN + phdr_size);
	header->paytype = 96;
	header->version = 2;
	header->seq_number = random();
	header->timestamp = random();

	uint8_t *data = (uint8_t *)&header->csrc[header->cc];

	if (phdr != NULL)
		*phdr = data;

	return data + phdr_size;
}

/**
 * Validate RTP header and get payload.
 *
 * @param hdr The pointer to data with RTP header to validate.
 * @param seq_number The pointer to a local RTP sequence number.
 * @return On success, this function returns pointer to data just after
 *   the RTP header - RTP header payload. On failure, NULL is returned. */
void *a2dp_rtp_payload(const rtp_header_t *hdr, uint16_t *seq_number) {

#if ENABLE_PAYLOADCHECK
	if (hdr->paytype < 96) {
		warn("Unsupported RTP payload type: %u", hdr->paytype);
		return NULL;
	}
#endif

	uint16_t loc_seq_number = ++*seq_number;
	uint16_t hdr_seq_number = be16toh(hdr->seq_number);

	if (hdr_seq_number != loc_seq_number) {
		if (loc_seq_number != 1)
			warn("Missing RTP packet: %u != %u", hdr_seq_number, loc_seq_number);
		*seq_number = hdr_seq_number;
	}

	return (void *)&hdr->csrc[hdr->cc];
}
