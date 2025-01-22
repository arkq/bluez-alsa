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

#include "resampler.h"

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <endian.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/param.h>
#include <sys/time.h>

#include <alsa/asoundlib.h>
#include <samplerate.h>

#include "shared/log.h"
#include "shared/ffb.h"
#include "shared/rt.h"

/* How many milliseconds to allow the delay to change before adjusting the
 * resampling rate. This value must allow the delay to vary due to timer
 * jitter without triggering a rate change. */
# define RESAMPLER_TOLERANCE_MS 3

/* How many milliseconds to wait for the delay value to stabilize after a
 * reset. */
#define RESAMPLER_STABILIZE_MS 5000

/* Step size of rate adjustment. */
#define RESAMPLER_STEP_SIZE 0.000004

/* Limit how many increment steps can be made when adjusting rate ratio. */
#define RESAMPLER_MAX_STEPS 100

/* Ignore rapid changes in delay since such changes can only result from
 * stream discontinuities, not timer drift. */
#define RESAMPLER_MAX_CHANGE_MS 10

/* Minimum time in milliseconds between rate ratio adjustments. */
#define RESAMPLER_PERIOD_MS 100

/* Number of samples to process in one go in case where the format
 * conversion is required. */
#define RESAMPLER_BUFFER_SIZE 4096

static const struct timespec ts_stabilize = {
	.tv_sec = RESAMPLER_STABILIZE_MS / 1000,
	.tv_nsec = (RESAMPLER_STABILIZE_MS % 1000) * 1000000,
};

/**
 * Check whether audio format is supported as input by the resampler. */
bool resampler_is_input_format_supported(snd_pcm_format_t format) {
	return format == SND_PCM_FORMAT_S16_LE ||
		format == SND_PCM_FORMAT_S32_LE ||
		format == SND_PCM_FORMAT_S24_LE;
}

/**
 * Check whether audio format is supported as output by the resampler. */
bool resampler_is_output_format_supported(snd_pcm_format_t format) {
	return format == SND_PCM_FORMAT_S16 ||
		format == SND_PCM_FORMAT_S32 ||
		format == SND_PCM_FORMAT_FLOAT;
}

/**
 * Initialize the resampler structure.
 *
 * @param resampler The resampler structure to initialize.
 * @param type The type of the converter to use.
 * @param channels The number of channels in the stream.
 * @param in_format The sample format of the incoming stream.
 * @param in_rate The nominal sample rate of the incoming stream.
 * @param out_format The required output sample format.
 * @param out_rate The nominal sample rate of the output stream.
 * @param min_target The minimum target delay.
 * @param max_target The maximum target delay.
 * @return On success this function returns 0. Otherwise -1 is returned. */
int resampler_init(
		struct resampler *resampler,
		enum resampler_converter_type type,
		unsigned int channels,
		snd_pcm_format_t in_format,
		unsigned int in_rate,
		snd_pcm_format_t out_format,
		unsigned int out_rate,
		snd_pcm_uframes_t min_target,
		snd_pcm_uframes_t max_target) {

	debug("Initializing resampler: min-delay=%#.1f max-delay=%#.1f",
			1000.0 * min_target / in_rate, 1000.0 * max_target / in_rate);

	/* Check whether formats can be resampled. */
	if (!resampler_is_input_format_supported(in_format) ||
			!resampler_is_output_format_supported(out_format)) {
		return errno = EINVAL, -1;
	}

	int err;
	if ((resampler->src_state = src_new(type, channels, &err)) == NULL) {
		debug("Couldn't create converter: %s", src_strerror(err));
		return errno = EINVAL, -1;
	}

	resampler->in_buffer = NULL;
	if (in_format != SND_PCM_FORMAT_FLOAT) {
		if ((resampler->in_buffer = malloc(RESAMPLER_BUFFER_SIZE * sizeof(float))) == NULL)
			goto fail;
	}

	resampler->out_buffer = NULL;
	if (out_format != SND_PCM_FORMAT_FLOAT) {
		if ((resampler->out_buffer = malloc(RESAMPLER_BUFFER_SIZE * sizeof(float))) == NULL)
			goto fail;
	}

	resampler->channels = channels;
	resampler->in_format = in_format;
	resampler->out_format = out_format;
	resampler->min_target = min_target;
	resampler->max_target = max_target;
	resampler->max_delay_diff = RESAMPLER_MAX_CHANGE_MS * in_rate / 1000;
	resampler->rate_ratio_step_count = 0;
	resampler->delay_tolerance = RESAMPLER_TOLERANCE_MS * in_rate / 1000;
	resampler->nominal_rate_ratio = (double)out_rate / (double)in_rate;
	resampler->steady_rate_ratio_step_count = 0;
	resampler->src_data.src_ratio = resampler->nominal_rate_ratio;
	resampler->input_frames = 0;
	resampler->last_input_frames = 0;
	resampler->period = RESAMPLER_PERIOD_MS * in_rate / 1000;
	resampler->in_rate = in_rate;

	return 0;

fail:
	resampler_free(resampler);
	return -1;
}

/**
 * Free resources allocated for the resampler. */
void resampler_free(struct resampler *resampler) {
	if (resampler->src_state != NULL) {
		src_delete(resampler->src_state);
		resampler->src_state = NULL;
	}
	free(resampler->in_buffer);
	resampler->in_buffer = NULL;
	free(resampler->out_buffer);
	resampler->out_buffer = NULL;
}

/**
 * Resample as many frames as possible from the buffer in to the buffer out. */
int resampler_process(struct resampler *resampler, ffb_t *in, ffb_t *out) {

	const unsigned int channels = resampler->channels;
	const size_t in_samples_total = ffb_len_out(in);
	SRC_DATA *src_data = &resampler->src_data;
	size_t samples_used = 0;

	/* If the input sample format is FLOAT we can process them directly
	 * without any intermediate conversion. Otherwise, we need to use our
	 * internal buffer to convert the samples to FLOAT before processing. */
	if (resampler->in_format != SND_PCM_FORMAT_FLOAT)
		src_data->data_in = resampler->in_buffer;
	else
		src_data->data_in = in->data;

	/* The same case applies to the output sample format. */
	if (resampler->out_format != SND_PCM_FORMAT_FLOAT)
		src_data->data_out = resampler->out_buffer;
	else
		src_data->data_out = out->tail;

	while (true) {

		size_t in_samples = in_samples_total - samples_used;
		size_t out_samples = ffb_len_in(out);

		/* Convert input samples to FLOAT format if necessary. */
		if (resampler->in_format != SND_PCM_FORMAT_FLOAT) {

			in_samples = MIN(in_samples, RESAMPLER_BUFFER_SIZE);

			if (resampler->in_format == SND_PCM_FORMAT_S16_LE) {
				const int16_t *in_data = in->data;
				src_short_to_float_array(in_data + samples_used, resampler->in_buffer, in_samples);
			}
			else if (resampler->in_format == SND_PCM_FORMAT_S32_LE) {
				const int32_t *in_data = in->data;
				src_int_to_float_array(in_data + samples_used, resampler->in_buffer, in_samples);
			}

		}

		if (resampler->out_format != SND_PCM_FORMAT_FLOAT)
			out_samples = MIN(out_samples, RESAMPLER_BUFFER_SIZE);

		src_data->input_frames = in_samples / channels;
		src_data->output_frames = out_samples / channels;

		int err;
		if ((err = src_process(resampler->src_state, src_data)) != 0) {
			error("Resampling failed: %s", src_strerror(err));
			return err;
		}

		if (src_data->output_frames_gen == 0)
			break;

		const size_t samples_gen = src_data->output_frames_gen * channels;
		samples_used += src_data->input_frames_used * channels;

		/* Convert output samples to integer format if necessary. */
		if (resampler->out_format == SND_PCM_FORMAT_S16)
			src_float_to_short_array(resampler->out_buffer, out->tail, samples_gen);
		else if (resampler->out_format == SND_PCM_FORMAT_S32)
			src_float_to_int_array(resampler->out_buffer, out->tail, samples_gen);

		ffb_seek(out, samples_gen);

	}

	ffb_shift(in, samples_used);
	return 0;
}

/**
 * Reset the resampling ratio to its nominal rate.
 *
 * This function should be called after any discontinuity in the stream. */
void resampler_reset(struct resampler *resampler) {
	resampler->src_data.src_ratio = resampler->nominal_rate_ratio;
	resampler->rate_ratio_step_count = 0;
	resampler->steady_rate_ratio_step_count = 0;
	/* Disable adaptive resampling until the delay has had time to settle. */
	resampler->target_delay = 0;
	gettimestamp(&resampler->reset_ts);
}

double resampler_current_rate_ratio(const struct resampler *resampler) {
	return resampler->src_data.src_ratio;
}

/**
 * Change the rate ratio applied by the resampler to adjust for the given new
 * delay value, always trying to move the delay back towards the target value.
 *
 * @return Returns true if the rate ratio was changed. */
bool resampler_update_rate_ratio(
		struct resampler *resampler,
		snd_pcm_uframes_t frames_read,
		snd_pcm_uframes_t delay) {

	bool ret = false;

	/* Update the rate ratio only if at least one period
	 * has passed since the last update. */
	if (frames_read > 0) {
		resampler->input_frames += frames_read;
		/* Prevent the possibility of integer overflow. */
		resampler->input_frames %= INTMAX_MAX;
		if (resampler->input_frames - resampler->last_input_frames < resampler->period)
			return false;
		resampler->last_input_frames = resampler->input_frames;
	}

	if (resampler->target_delay == 0 && !is_timespec_zero(&resampler->reset_ts)) {
		/* Do not restart adaptive resampling until the delay has had time to
		 * settle to a new value. */
		struct timespec ts_wait;
		struct timespec ts_now;
		gettimestamp(&ts_now);
		timespecadd(&resampler->reset_ts, &ts_stabilize, &ts_wait);
		if (difftimespec(&ts_now, &ts_wait, &ts_wait) < 0) {
			/* Do not allow the target to be outside the configured range. If
			 * the actual delay is outside that range then try to move it back
			 * as quickly as possible. */
			if (delay > resampler->max_target) {
				resampler->target_delay = resampler->max_target;
				resampler->src_data.src_ratio =
					resampler->nominal_rate_ratio - RESAMPLER_STEP_SIZE * RESAMPLER_MAX_STEPS;
				resampler->rate_ratio_step_count = -RESAMPLER_MAX_STEPS;
			}
			else if (delay < resampler->min_target) {
				resampler->target_delay = resampler->min_target;
				resampler->src_data.src_ratio =
					resampler->nominal_rate_ratio + RESAMPLER_STEP_SIZE * RESAMPLER_MAX_STEPS;
				resampler->rate_ratio_step_count = RESAMPLER_MAX_STEPS;
			}
			else {
				/* Once the delay has returned within the permitted target
				 * range, zero the reset timestamp to indicate that normal
				 * adaptive resampling has re-started. */
				resampler->reset_ts.tv_sec = 0;
				resampler->reset_ts.tv_nsec = 0;
				resampler->target_delay = delay;
			}
			resampler->delay_diff = delay - resampler->target_delay;
			debug("Adaptive resampling target delay: %#.1f ms",
					1000.0 * resampler->target_delay / resampler->in_rate);
			return true;
		}
		return false;
	}

	snd_pcm_sframes_t delay_diff = delay - resampler->target_delay;
	snd_pcm_uframes_t delay_diff_abs = labs(delay_diff);

	/* Reset the resampler whenever the delay exceeds the limit. */
	if (delay_diff_abs > resampler->max_delay_diff) {
		/* Reset the resampler if not already done. */
		if (is_timespec_zero(&resampler->reset_ts)) {
#if DEBUG
			if (resampler->target_delay != 0)
				/* Omit the log message if the target delay is not yet set. In such
				 * case we are just initializing the resampler, not resetting it. */
				debug("Resetting resampler: Delay difference limit exceeded: %zu > %zu",
						delay_diff_abs, resampler->max_delay_diff);
#endif
			resampler_reset(resampler);
			return true;
		}
	}

	if (delay_diff_abs > resampler->delay_tolerance) {
		/* When the delay is not already moving back towards tolerance,
		 * step the size of the rate adjustment in the appropriate
		 * direction. */
		if (delay_diff > 0 && delay_diff > resampler->delay_diff) {
			if (resampler->rate_ratio_step_count > -RESAMPLER_MAX_STEPS) {
				resampler->src_data.src_ratio -= RESAMPLER_STEP_SIZE;
				resampler->rate_ratio_step_count--;
				ret = true;
			}
		}
		else if (delay_diff < 0 && delay_diff < resampler->delay_diff) {
			if (resampler->rate_ratio_step_count < RESAMPLER_MAX_STEPS) {
				resampler->src_data.src_ratio += RESAMPLER_STEP_SIZE;
				resampler->rate_ratio_step_count++;
				ret = true;
			}
		}
	}
	else if (labs(resampler->delay_diff) > resampler->delay_tolerance) {
		/* When the delay has returned to tolerance, step the size of the steady
		 * rate ratio in the appropriate direction and set the rate ratio to
		 * the revised steady rate ratio. */
		if (resampler->delay_diff > 0) {
			if (resampler->steady_rate_ratio_step_count > -RESAMPLER_MAX_STEPS) {
				resampler->steady_rate_ratio_step_count--;
				ret = true;
			}
		}
		else {
			if (resampler->steady_rate_ratio_step_count > RESAMPLER_MAX_STEPS) {
				resampler->steady_rate_ratio_step_count++;
				ret = true;
			}
		}
		if (ret) {
			resampler->rate_ratio_step_count = resampler->steady_rate_ratio_step_count;
			resampler->src_data.src_ratio =
				resampler->nominal_rate_ratio + RESAMPLER_STEP_SIZE * resampler->rate_ratio_step_count;
		}
	}

	resampler->delay_diff = delay_diff;

	return ret;
}

/**
 * Convert a buffer of PCM samples to the equivalent native-endian format. The
 * samples are modified in place. Also pad 24bit samples (packed into 32bits)
 * to convert them to valid 32-bit samples.
 *
 * @param buffer The buffer of samples to convert.
 * @param len The number of samples in the buffer.
 * @param format The original format of the samples. */
void resampler_convert_to_native_endian_format(
		void *buffer, size_t len, snd_pcm_format_t format) {
#if __BYTE_ORDER == __BIG_ENDIAN

	switch (format) {
	case SND_PCM_FORMAT_S16_LE: {
		uint16_t *data = buffer;
		for (size_t n = 0; n < len; n++)
			le16toh(data[n]);
	} break;
	case SND_PCM_FORMAT_S24_LE: {
		uint32_t *data = buffer;
		for (size_t n = 0; n < len; n++) {
			le32toh(data[n]);
			/* convert to S32 */
			data[n] <<= 8;
		}
	} break;
	case SND_PCM_FORMAT_S32_LE: {
		uint32_t *data = buffer;
		for (size_t n = 0; n < len; n++)
			le32toh(data[n]);
	} break;
	default:
		return;
	}

#else

	switch (format) {
	case SND_PCM_FORMAT_S24_LE: {
		uint32_t *data = buffer;
		for (size_t n = 0; n < len; n++) {
			/* convert to S32 */
			data[n] <<= 8;
		}
	} break;
	default:
		return;
	}

#endif
}

/**
 * Return the equivalent native-endian format for the given format.
 * For unsupported formats the given source format value is returned. */
snd_pcm_format_t resampler_native_endian_format(snd_pcm_format_t format) {
#if __BYTE_ORDER == __BIG_ENDIAN
	switch (format) {
	case SND_PCM_FORMAT_S16_LE:
		return SND_PCM_FORMAT_S16;
	case SND_PCM_FORMAT_S24_LE:
		/* 24-bit samples must be converted to 32-bit
		 * before passing to the resampler. */
		return SND_PCM_FORMAT_S32;
	case SND_PCM_FORMAT_S32_LE:
		return SND_PCM_FORMAT_S32;
	default:
		return format;
	}
#else
	if (format == SND_PCM_FORMAT_S24_LE)
		/* 24-bit samples must be converted to 32-bit
		 * before passing to the resampler. */
		return SND_PCM_FORMAT_S32;
	return format;
#endif
}

snd_pcm_format_t resampler_preferred_output_format(void) {
	return SND_PCM_FORMAT_FLOAT;
}
