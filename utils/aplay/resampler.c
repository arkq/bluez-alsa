/*
 * BlueALSA - resampler.c
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
 * Copyright (c) 2025 borine
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <alsa/asoundlib.h>
#include <samplerate.h>

#include <endian.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/param.h>

#include "shared/log.h"
#include "shared/ffb.h"
#include "shared/rt.h"

#include "resampler.h"

/* How many milliseconds to allow the delay to change before adjusting the
 * resampling rate. This value must allow the delay to vary due to timer
 * jitter without triggering a rate change. */
# define RESAMPLER_TOLERANCE_MS 10

/* How many milliseconds to wait for the delay value to stabilize after a
 * reset. */
#define RESAMPLER_STABILIZE_MS 5000

/* Step size of rate adjustment */
#define RESAMPLER_STEP_SIZE 0.000003

/* Limit how many increment steps can be made when adjusting rate ratio. */
#define RESAMPLER_MAX_STEPS 60

/* Ignore rapid changes in delay since such changes can only result from
 * stream discontinuities, not timer drift. */
#define RESAMPLER_MAX_CHANGE_MS 10

struct aplay_resampler {
	SRC_STATE *src_state;
	SRC_DATA src_data;
	float *in_buffer;
	float *out_buffer;
	unsigned int channels;
	snd_pcm_format_t in_format;
	snd_pcm_format_t out_format;
	snd_pcm_uframes_t max_frames;
	double nominal_rate_ratio;
	int rate_ratio_step_count;
	snd_pcm_uframes_t target_delay;
	snd_pcm_uframes_t delay_tolerance;
	snd_pcm_sframes_t delay_diff;
	snd_pcm_sframes_t max_delay_diff;
	struct timespec reset_ts;
};

static const struct timespec ts_stabilize = {
	.tv_sec = RESAMPLER_STABILIZE_MS / 1000,
	.tv_nsec = (RESAMPLER_STABILIZE_MS % 1000) * 1000000,
};

/**
 * The ALSA audio formats supported as output by the resampler */
static bool resampler_supports_output_format(snd_pcm_format_t format) {
	return format == SND_PCM_FORMAT_S16 ||
		format == SND_PCM_FORMAT_S32 ||
		format == SND_PCM_FORMAT_FLOAT;
}

/**
 * The Bluetooth audio formats supported as input by the resampler */
bool resampler_supports_input_format(snd_pcm_format_t format) {
	return format == SND_PCM_FORMAT_S16_LE ||
		format == SND_PCM_FORMAT_S32_LE ||
		format == SND_PCM_FORMAT_S24_LE;
}

void resampler_delete(struct aplay_resampler *resampler) {
	if (resampler == NULL)
		return;
	if (resampler->src_state != NULL)
		src_delete(resampler->src_state);
	free(resampler->in_buffer);
	free(resampler->out_buffer);
	free(resampler);
}

struct aplay_resampler *resampler_create(
			enum aplay_converter converter_type,
			unsigned int channels,
			snd_pcm_format_t in_format,
			unsigned int in_rate,
			snd_pcm_format_t out_format,
			unsigned int out_rate,
			snd_pcm_uframes_t max_frames) {

	/* Check formats can be resampled */
	if (!resampler_supports_input_format(in_format) || !resampler_supports_output_format(out_format)) {
		errno = EINVAL;
		return NULL;
	}

	struct aplay_resampler *resampler = calloc(1, sizeof(struct aplay_resampler));
	if (resampler == NULL) {
		debug("Unable to create resampler: %s", strerror(errno));
		return NULL;
	}

	int error;
	resampler->src_state = src_new(converter_type, channels, &error);
	if (resampler->src_state == NULL) {
		debug("Unable to create resampler: %s", src_strerror(error));
		goto fail;
	}

	if (in_format != SND_PCM_FORMAT_FLOAT) {
		resampler->in_buffer = calloc(max_frames * channels, sizeof(float));
		if (resampler->in_buffer == NULL)
			goto fail;
	}

	if (out_format != SND_PCM_FORMAT_FLOAT) {
		resampler->out_buffer = calloc(max_frames * channels, sizeof(float));
		if (resampler->out_buffer == NULL)
			goto fail;
	}

	resampler->channels = channels;
	resampler->in_format = in_format;
	resampler->out_format = out_format;
	resampler->max_frames = max_frames;
	resampler->max_delay_diff = RESAMPLER_MAX_CHANGE_MS * in_rate / 1000;
	resampler->rate_ratio_step_count = 0;
	resampler->delay_tolerance = RESAMPLER_TOLERANCE_MS * in_rate / 1000;
	resampler->nominal_rate_ratio = (double)out_rate / (double)in_rate;
	resampler->src_data.src_ratio = resampler->nominal_rate_ratio;

	return resampler;

fail:
	resampler_delete(resampler);
	return NULL;
}

int resampler_process(struct aplay_resampler *resampler, ffb_t *in, ffb_t *out) {
	int err = 0;
	SRC_DATA *src_data = &resampler->src_data;
	snd_pcm_uframes_t frames_used = 0;

	/* We must ensure that we only process as many samples as will fit into
	 * the out buffer. */
	size_t out_samples = ffb_len_in(out);
	size_t max_in_samples = out_samples / resampler->src_data.src_ratio;

	size_t in_samples = MIN(ffb_len_out(in), max_in_samples);

	if (resampler->in_format == SND_PCM_FORMAT_S16) {
		in_samples = MIN(in_samples, resampler->max_frames * resampler->channels);
		src_short_to_float_array(in->data, resampler->in_buffer, in_samples);
		src_data->data_in = resampler->in_buffer;
	}
	else if (resampler->in_format == SND_PCM_FORMAT_S32) {
		in_samples = MIN(in_samples, resampler->max_frames * resampler->channels);
		src_int_to_float_array(in->data, resampler->in_buffer, in_samples);
		src_data->data_in = resampler->in_buffer;
	}
	else {
		src_data->data_in = in->data;
	}
	src_data->input_frames = in_samples / resampler->channels;

	if (resampler->out_format == SND_PCM_FORMAT_FLOAT)
		src_data->data_out = out->tail;
	else
		src_data->data_out = resampler->out_buffer;
	src_data->output_frames = out_samples / resampler->channels;

	while (true) {
		if ((err = src_process(resampler->src_state, src_data)) != 0) {
			error("Resampler failed: %s", src_strerror(err));
			return err;
		}
		if (src_data->output_frames_gen == 0)
			break;

		src_data->data_in += src_data->input_frames_used * resampler->channels;
		src_data->input_frames -= src_data->input_frames_used;
		frames_used += src_data->input_frames_used;
		src_data->output_frames -= src_data->output_frames_gen;

		if (resampler->out_format == SND_PCM_FORMAT_S16)
			src_float_to_short_array(resampler->out_buffer, out->tail, src_data->output_frames_gen * resampler->channels);
		else if (resampler->out_format == SND_PCM_FORMAT_S32)
			src_float_to_int_array(resampler->out_buffer, out->tail, src_data->output_frames_gen * resampler->channels);

		ffb_seek(out, src_data->output_frames_gen * resampler->channels);
	}
	ffb_shift(in, frames_used * resampler->channels);

	return err;
}

/**
 * Change the rate ratio applied by the resampler to adjust for the given new
 * delay value, always trying to move the delay back towards the target value
 * @return true if the rate ratio was changed. */
bool resampler_update_rate_ratio(
			struct aplay_resampler *resampler,
			snd_pcm_uframes_t delay) {

	/* The direction of rate change required to return the delay to tolerance */
	enum resampler_rate_change {
		RATE_DECREASE = -1,
		RATE_NOCHANGE = 0,
		RATE_INCREASE = 1,
	} rate_change = RATE_NOCHANGE;

	/* Check if we need to re-enable adaptive resampling after a reset */
	if (resampler->target_delay == 0) {
		struct timespec ts_now;
		struct timespec ts_wait;
		timespecadd(&resampler->reset_ts, &ts_stabilize, &ts_wait);
		gettimestamp(&ts_now);
		if (difftimespec(&ts_now, &ts_wait, &ts_wait) < 0) {
			resampler->target_delay = delay;
			return true;
		}
		else
			return false;
	}

	snd_pcm_sframes_t delay_diff = delay - resampler->target_delay;

	/* Ignore a delay change that exceeds the limit. */
	if (labs(delay_diff - resampler->delay_diff) > resampler->max_delay_diff)
		return false;

	if (labs(delay_diff) > resampler->delay_tolerance) {
		/* When the delay is not already moving back towards tolerance,
		 * step the size of the rate adjustment in the appropriate
		 * direction */
		if (delay_diff > 0 && delay_diff > resampler->delay_diff)
			rate_change = RATE_DECREASE;
		else if (delay_diff < 0 && delay_diff < resampler->delay_diff)
			rate_change = RATE_INCREASE;
	}
	else if (labs(resampler->delay_diff) > resampler->delay_tolerance) {
		/* When the delay has returned to tolerance, step the size of the rate
		 * adjustment back towards the nominal rate to reduce the amount of
		 * "overshoot". */
		if (resampler->delay_diff > 0)
			rate_change = RATE_INCREASE;
		else
			rate_change = RATE_DECREASE;
	}

	switch(rate_change) {
	case RATE_INCREASE:
		if (resampler->rate_ratio_step_count < RESAMPLER_MAX_STEPS) {
			resampler->src_data.src_ratio += RESAMPLER_STEP_SIZE;
			resampler->rate_ratio_step_count++;
		}
		else
			rate_change = RATE_NOCHANGE;
		break;
	case RATE_DECREASE:
		if (resampler->rate_ratio_step_count > -RESAMPLER_MAX_STEPS) {
			resampler->src_data.src_ratio -= RESAMPLER_STEP_SIZE;
			resampler->rate_ratio_step_count--;
		}
		else
			rate_change = RATE_NOCHANGE;
		break;
	default:
		break;
	}

	resampler->delay_diff = delay_diff;
	return rate_change != RATE_NOCHANGE;
}

/**
 * Reset the resampling ratio to its nominal rate after any discontinuity in
 * the stream. */
void resampler_reset(struct aplay_resampler *resampler) {
	resampler->src_data.src_ratio = resampler->nominal_rate_ratio;
	resampler->target_delay = 0;
	gettimestamp(&resampler->reset_ts);
}

double resampler_current_rate_ratio(struct aplay_resampler *resampler) {
	return resampler->src_data.src_ratio;
}

bool resampler_ready(struct aplay_resampler *resampler) {
	return resampler->target_delay != 0;
}

#if __BYTE_ORDER == __LITTLE_ENDIAN

/**
 * Convert a buffer of little-endian samples to the equivalent native-endian
 * format. The samples are modified in place. Also pad 24bit samples (packed
 * into 32bits) to convert them to valid 32-bit samples.
 * On little-endian hosts this function only modifies 24-bit samples.
 * @param len the number of **samples** in the buffer
 * @param format the original format of the samples. */
void resampler_format_le_to_native(void *buffer, size_t len, snd_pcm_format_t format) {
	if (format == SND_PCM_FORMAT_S24_LE) {
		/* Convert to S32 */
		uint32_t *data = buffer;
		for (size_t n = 0; n < len; n++) {
			if (data[n] & 0x00800000)
				data[n] |= 0xff000000;
			else
				data[n] &= 0x00ffffff;
		}
	}
}

#elif __BYTE_ORDER == __BIG_ENDIAN
#include <byteswap.h>

/**
 * Convert a buffer of little-endian samples to the equivalent native-endian
 * format. The samples are modified in place. Also pad 24bit samples (packed
 * into 32bits) to convert them to valid 32-bit samples.
 * @param len the number of **samples** in the buffer
 * @param format the original format of the samples. */
void resampler_format_le_to_native(void *buffer, size_t len, snd_pcm_format_t format) {
	size_t n;

	switch (format) {
	case SND_PCM_FORMAT_S16_LE:
		uint16_t *data = buffer;
		for (n = 0; n < len; n++)
			bswap_16(data[n]);
		break;
	case SND_PCM_FORMAT_S24_LE:
		uint32_t *data = buffer;
		/* Convert to S32 */
		for (n = 0; n < len; n++) {
			if (data[n] & 0x00008000)
				data[n] |= 0x000000ff;
			else
				data[n] &= 0xffffff00;
			bswap_32(data[n]);
		}
		break;
	case SND_PCM_FORMAT_S32_LE:
		uint32_t *data = buffer;
		for (n = 0; n < len; n++)
			bswap_32(data[n]);
		break;
	default:
		return;
	}
}

/**
 * Return the equivalent supported native-endian format for the given source
 * format. For unsupported formats the given source format value is returned. */
snd_pcm_format_t resampler_native_format(snd_pcm_format_t source_format) {
	switch (source_format) {
	case SND_PCM_FORMAT_S16_LE:
		return SND_PCM_FORMAT_S16;
	case SND_PCM_FORMAT_S24_LE:
		/* 24bit samples must be converted to 32 bit before passing to
		 * the resampler */
		return SND_PCM_FORMAT_S32;
	case SND_PCM_FORMAT_S32_LE:
		return SND_PCM_FORMAT_S32;
	default:
		return source_format;
	}
}

#else
# error "Unknown byte order"
#endif
