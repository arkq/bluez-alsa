/*
 * BlueALSA - codec-lc3-swb.h
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_CODECLC3SWB_H_
#define BLUEALSA_CODECLC3SWB_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <lc3.h>

#include "h2.h"
#include "shared/ffb.h"

/* LC3-SWB uses LC3 encoding with precisely defined parameters: mono, 32 kHz
 * sample rate, 7.5 ms frame duration. Hence, the size of the input (number
 * of PCM samples) and output is known up front. */
#define LC3_SWB_CODESIZE    240 * sizeof(int16_t)
#define LC3_SWB_CODESAMPLES (LC3_SWB_CODESIZE / sizeof(int16_t))
#define LC3_SWB_FRAMELEN    58

typedef struct h2_lc3_swb_frame {
	h2_header_t header;
	uint8_t payload[LC3_SWB_FRAMELEN];
} h2_lc3_swb_frame_t;

static_assert(
	sizeof(h2_lc3_swb_frame_t) == sizeof(h2_header_t) + LC3_SWB_FRAMELEN,
	"Incorrect LC3-SWB H2 frame size");

struct esco_lc3_swb {

	/* encoder/decoder */
	lc3_encoder_t encoder;
	lc3_decoder_t decoder;

	/* buffer for eSCO frames */
	ffb_t data;
	/* buffer for PCM samples */
	ffb_t pcm;

	bool seq_initialized;
	uint8_t seq_number : 2;
	/* number of processed frames */
	size_t frames;

	/* Allocated memory for LC3 encoder and decoder. */
	LC3_ENCODER_MEM_T(7500, 32000) mem_encoder;
	LC3_DECODER_MEM_T(7500, 32000) mem_decoder;

	/* Allocated buffer for 3 LC3-SWB frames to have some extra space in case of
	 * PCM samples asynchronous reading beeing slower than incoming frames. */
	uint8_t buffer_data[sizeof(h2_lc3_swb_frame_t) * 3];
	/* Allocate buffer for 1 decoded frame, optional 3 PLC frames and some
	 * extra frames to account for async PCM samples reading. */
	int16_t buffer_pcm[LC3_SWB_CODESAMPLES * 6];

};

void lc3_swb_init(struct esco_lc3_swb *lc3_swb);

ssize_t lc3_swb_get_delay(struct esco_lc3_swb *lc3_swb);
ssize_t lc3_swb_encode(struct esco_lc3_swb *lc3_swb);
ssize_t lc3_swb_decode(struct esco_lc3_swb *lc3_swb);

#endif
