/*
 * BlueALSA - resampler.h
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
 * Copyright (c) 2025 borine
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_APLAY_RESAMPLER_H_
#define BLUEALSA_APLAY_RESAMPLER_H_

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include <alsa/asoundlib.h>
#include <samplerate.h>

#include "shared/ffb.h"

struct resampler {

	/* The state and configuration data. */
	SRC_STATE *src_state;
	SRC_DATA src_data;

	/* Internal buffers for converting from
	 * integer to float sample formats. */
	float *in_buffer;
	float *out_buffer;

	/* The number of channels of the stream. */
	unsigned int channels;

	/* input sample format */
	snd_pcm_format_t in_format;
	/* output sample format */
	snd_pcm_format_t out_format;

	/* lower bound on the selected target delay */
	snd_pcm_uframes_t min_target;
	/* upper bound on the selected target delay */
	snd_pcm_uframes_t max_target;
	/* conversion ratio assuming zero timer drift */
	double nominal_rate_ratio;
	/* how many steps above or below nominal rate ratio for next processing iteration */
	int rate_ratio_step_count;
	/* current best estimate of step count to give steady delay value */
	int steady_rate_ratio_step_count;
	/* delay value that conversion tries to achieve */
	snd_pcm_uframes_t target_delay;
	/* variation in delay tolerated without changing number of steps */
	snd_pcm_uframes_t delay_tolerance;
	/* difference between delay and target delay at last processing iteration */
	snd_pcm_sframes_t delay_diff;
	/* upper bound on absolute delay difference before automatic reset */
	snd_pcm_uframes_t max_delay_diff;
	/* total number of input frames processed */
	uintmax_t input_frames;
	/* total number of input frames at time of last rate ratio update */
	uintmax_t last_input_frames;
	/* minimum number of input frames between rate ratio updates */
	snd_pcm_uframes_t period;
	/* timestamp of last resampler reset */
	struct timespec reset_ts;
	/* nominal sample rate of the incoming stream */
	unsigned int in_rate;

};

enum resampler_converter_type {
	RESAMPLER_CONV_NONE                   = -1,
	RESAMPLER_CONV_SINC_BEST_QUALITY      = SRC_SINC_BEST_QUALITY,
	RESAMPLER_CONV_SINC_MEDIUM_QUALITY    = SRC_SINC_MEDIUM_QUALITY,
	RESAMPLER_CONV_SINC_FASTEST           = SRC_SINC_FASTEST,
	RESAMPLER_CONV_ZERO_ORDER_HOLD        = SRC_ZERO_ORDER_HOLD,
	RESAMPLER_CONV_LINEAR                 = SRC_LINEAR,
};

bool resampler_is_input_format_supported(snd_pcm_format_t format);
bool resampler_is_output_format_supported(snd_pcm_format_t format);

int resampler_init(
		struct resampler *resampler,
		enum resampler_converter_type type,
		unsigned int channels,
		snd_pcm_format_t in_format,
		unsigned int in_rate,
		snd_pcm_format_t out_format,
		unsigned int out_rate,
		snd_pcm_uframes_t min_target,
		snd_pcm_uframes_t max_target);

void resampler_free(
		struct resampler *resampler);

int resampler_process(
		struct resampler *resampler,
		ffb_t *in,
		ffb_t *out);

void resampler_reset(
		struct resampler *resampler);

double resampler_current_rate_ratio(
		const struct resampler *resampler);

bool resampler_update_rate_ratio(
		struct resampler *resampler,
		snd_pcm_uframes_t frames_read,
		snd_pcm_uframes_t delay);

void resampler_convert_to_native_endian_format(
		void *buffer, size_t len, snd_pcm_format_t format);

snd_pcm_format_t resampler_native_endian_format(snd_pcm_format_t format);
snd_pcm_format_t resampler_preferred_output_format(void);

#endif
