/*
 * BlueALSA - a2dp-rtp.h
 * Copyright (c) 2016 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_A2DPRTP_H_
#define BLUEALSA_A2DPRTP_H_

#include <stdint.h>
#include <ortp/rtp.h>

/**
 * Media payload header for SBC. */
typedef struct rtp_payload_sbc {
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
} __attribute__ ((packed)) rtp_payload_sbc_t;

#endif
