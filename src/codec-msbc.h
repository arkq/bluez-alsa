/*
 * BlueALSA - codec-msbc.h
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_CODECMSBC_H_
#define BLUEALSA_CODECMSBC_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <sbc/sbc.h>
#include <spandsp.h>

#include "h2.h"
#include "shared/ffb.h"

/* HFP uses SBC encoding with precisely defined parameters. Hence, the size
 * of the input (number of PCM samples) and output is known up front. */
#define MSBC_CODESIZE    240
#define MSBC_CODESAMPLES (MSBC_CODESIZE / sizeof(int16_t))
#define MSBC_FRAMELEN    57

typedef struct h2_msbc_frame {
	h2_header_t header;
	uint8_t payload[MSBC_FRAMELEN];
	uint8_t padding;
} __attribute__ ((packed)) h2_msbc_frame_t;

struct esco_msbc {

	/* encoder/decoder */
	sbc_t sbc;

	/* buffer for eSCO frames */
	ffb_t data;
	/* buffer for PCM samples */
	ffb_t pcm;

	bool seq_initialized;
	uint8_t seq_number : 2;
	/* number of processed frames */
	size_t frames;

	/* packet loss concealment */
	plc_state_t *plc;

	/* Determine whether structure has been initialized. This field is
	 * used for reinitialization - it makes msbc_init() idempotent. */
	bool initialized;

	/* Allocated buffer for 3 mSBC frames to have some extra space in case of
	 * PCM samples asynchronous reading beeing slower than incoming frames. */
	uint8_t buffer_data[sizeof(h2_msbc_frame_t) * 3];
	/* Allocate buffer for 1 decoded frame, optional 3 PLC frames and
	 * some extra frames to account for async PCM samples reading. */
	int16_t buffer_pcm[MSBC_CODESAMPLES * 6];

};

int msbc_init(struct esco_msbc *msbc);
void msbc_finish(struct esco_msbc *msbc);

ssize_t msbc_decode(struct esco_msbc *msbc);
ssize_t msbc_encode(struct esco_msbc *msbc);

const char *msbc_strerror(int err);

#endif
