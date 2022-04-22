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
void *rtp_a2dp_init(void *s, rtp_header_t **hdr, void **phdr, size_t phdr_size) {

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
 * Check A2DP RTP header sequence number.
 *
 * @param hdr The pointer to data with RTP header.
 * @param counter The pointer to a local RTP sequence number counter.
 * @return This function returns the number of missing RTP frames. */
uint16_t rtp_a2dp_check_sequence(const rtp_header_t *hdr, uint16_t *counter) {

	uint16_t loc_seq_number = ++*counter;
	uint16_t hdr_seq_number = be16toh(hdr->seq_number);
	uint16_t missing = hdr_seq_number - loc_seq_number;

	if (missing != 0) {
		if (loc_seq_number == 1)
			/* Do not report missing frames if the counter was set to zero prior
			 * to the call to this function - counter initialization. */
			missing = 0;
		else
			warn("Missing RTP packets [%u != %u]: %u",
					hdr_seq_number, loc_seq_number, missing);
		*counter = hdr_seq_number;
	}

	return missing;
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
