/*
 * BlueALSA - alsa-pcm.c
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "alsa-pcm.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shared/log.h"

static int alsa_pcm_set_hw_params(
		struct alsa_pcm *pcm,
		snd_pcm_format_t format_1,
		snd_pcm_format_t format_2,
		snd_pcm_format_t *selected_format,
		unsigned int channels,
		unsigned int *rate,
		bool exact_rate,
		unsigned int *buffer_time,
		unsigned int *period_time,
		char **msg) {

	const snd_pcm_access_t access = SND_PCM_ACCESS_RW_INTERLEAVED;
	snd_pcm_format_t format = SND_PCM_FORMAT_UNKNOWN;
	snd_pcm_t *snd_pcm = pcm->pcm;
	snd_pcm_hw_params_t *params;
	char buf[256];
	int dir;
	int err;

	snd_pcm_hw_params_alloca(&params);

	if ((err = snd_pcm_hw_params_any(snd_pcm, params)) < 0) {
		snprintf(buf, sizeof(buf), "Set all possible ranges: %s", snd_strerror(err));
		goto fail;
	}

	if ((err = snd_pcm_hw_params_set_access(snd_pcm, params, access)) != 0) {
		snprintf(buf, sizeof(buf), "Set assess type: %s: %s", snd_strerror(err), snd_pcm_access_name(access));
		goto fail;
	}

	/* Prefer format_1 if supported by the device. */
	if ((err = snd_pcm_hw_params_set_format(snd_pcm, params, format_1)) == 0)
		format = format_1;
	/* Otherwise try format 2 */
	else if (format_2 != SND_PCM_FORMAT_UNKNOWN) {
		if ((err = snd_pcm_hw_params_set_format(snd_pcm, params, format_2)) == 0)
			format = format_2;
		else {
			snprintf(buf, sizeof(buf), "Set format: %s: %s and %s", snd_strerror(err), snd_pcm_format_name(format_1), snd_pcm_format_name(format_2));
			goto fail;
		}
	}
	else {
		snprintf(buf, sizeof(buf), "Set format: %s: %s", snd_strerror(err), snd_pcm_format_name(format_1));
		goto fail;
	}

	*selected_format = format;

	if ((err = snd_pcm_hw_params_set_channels(snd_pcm, params, channels)) != 0) {
		snprintf(buf, sizeof(buf), "Set channels: %s: %d", snd_strerror(err), channels);
		goto fail;
	}

	dir = 0;
	if (exact_rate)
		err = snd_pcm_hw_params_set_rate(snd_pcm, params, *rate, dir);
	else
		err = snd_pcm_hw_params_set_rate_near(snd_pcm, params, rate, &dir);
	if (err != 0) {
		snprintf(buf, sizeof(buf), "Set sample rate: %s: %u", snd_strerror(err), *rate);
		goto fail;
	}

	dir = 0;
	if ((err = snd_pcm_hw_params_set_period_time_near(snd_pcm, params, period_time, &dir)) != 0) {
		snprintf(buf, sizeof(buf), "Set period time: %s: %u", snd_strerror(err), *period_time);
		goto fail;
	}

	dir = 0;
	if ((err = snd_pcm_hw_params_set_buffer_time_near(snd_pcm, params, buffer_time, &dir)) != 0) {
		snprintf(buf, sizeof(buf), "Set buffer time: %s: %u", snd_strerror(err), *buffer_time);
		goto fail;
	}

	if ((err = snd_pcm_hw_params(snd_pcm, params)) != 0) {
		snprintf(buf, sizeof(buf), "%s", snd_strerror(err));
		goto fail;
	}

	return 0;

fail:
	*selected_format = format;
	if (msg != NULL)
		*msg = strdup(buf);
	return err;
}

static int alsa_pcm_set_sw_params(
		struct alsa_pcm *pcm,
		snd_pcm_uframes_t start_threshold,
		char **msg) {

	snd_pcm_t *snd_pcm = pcm->pcm;
	snd_pcm_sw_params_t *params;
	char buf[256];
	int err;

	snd_pcm_sw_params_alloca(&params);

	if ((err = snd_pcm_sw_params_current(snd_pcm, params)) != 0) {
		snprintf(buf, sizeof(buf), "Get current sw params: %s", snd_strerror(err));
		goto fail;
	}

	if ((err = snd_pcm_sw_params_set_start_threshold(snd_pcm, params, start_threshold)) != 0) {
		snprintf(buf, sizeof(buf), "Set start threshold: %s: %lu", snd_strerror(err), start_threshold);
		goto fail;
	}

	if ((err = snd_pcm_sw_params(snd_pcm, params)) != 0) {
		snprintf(buf, sizeof(buf), "Set sw params: %s", snd_strerror(err));
		goto fail;
	}

	return 0;

fail:
	if (msg != NULL)
		*msg = strdup(buf);
	return err;
}

void alsa_pcm_init(struct alsa_pcm *pcm) {
	memset(pcm, 0, sizeof(*pcm));
}

int alsa_pcm_open(
		struct alsa_pcm *pcm,
		const char *name,
		snd_pcm_format_t format_1,
		snd_pcm_format_t format_2,
		unsigned int channels,
		unsigned int rate,
		unsigned int buffer_time,
		unsigned int period_time,
		int flags,
		char **msg) {

	char *tmp = NULL;
	char buf[256];
	unsigned int actual_buffer_time = buffer_time;
	unsigned int actual_period_time = period_time;
	snd_pcm_format_t actual_format = SND_PCM_FORMAT_UNKNOWN;
	unsigned int actual_rate = rate;
	const bool exact_rate = !(flags & SND_PCM_NO_AUTO_RESAMPLE);
	int err;

	if ((err = snd_pcm_open(&pcm->pcm, name, SND_PCM_STREAM_PLAYBACK, flags)) != 0) {
		snprintf(buf, sizeof(buf), "Open PCM: %s", snd_strerror(err));
		goto fail;
	}

	pcm->format = actual_format;
	if ((err = alsa_pcm_set_hw_params(pcm, format_1, format_2,
				&actual_format, channels, &actual_rate, exact_rate,
				&actual_buffer_time, &actual_period_time, &tmp)) != 0) {
		snprintf(buf, sizeof(buf), "Set HW params: %s", tmp);
		goto fail;
	}
	pcm->format = actual_format;

	snd_pcm_uframes_t buffer_size, period_size;
	if ((err = snd_pcm_get_params(pcm->pcm, &buffer_size, &period_size)) != 0) {
		snprintf(buf, sizeof(buf), "Get params: %s", snd_strerror(err));
		goto fail;
	}

	/* Start the transfer when three requested periods have been written (or
	 * when the buffer is full if it holds less than three requested periods. */
	snd_pcm_uframes_t start_threshold = ((snd_pcm_uframes_t)period_time * 3 / 1000) * (rate / 1000);
	if (start_threshold > buffer_size)
		start_threshold = buffer_size;

	if ((err = alsa_pcm_set_sw_params(pcm, start_threshold, &tmp)) != 0) {
		snprintf(buf, sizeof(buf), "Set SW params: %s", tmp);
		goto fail;
	}

	if ((err = snd_pcm_prepare(pcm->pcm)) != 0) {
		snprintf(buf, sizeof(buf), "Prepare: %s", snd_strerror(err));
		goto fail;
	}

	pcm->channels = channels;
	pcm->sample_size = snd_pcm_format_size(actual_format, 1);
	pcm->frame_size = snd_pcm_format_size(actual_format, channels);
	pcm->rate = actual_rate;
	pcm->buffer_time = actual_buffer_time;
	pcm->period_time = actual_period_time;
	pcm->buffer_frames = buffer_size;
	pcm->period_frames = period_size;
	pcm->start_threshold = start_threshold;
	pcm->delay = 0;
	pcm->hw_avail = 0;

	/* Maintain buffer fill level above 1 period. */
	pcm->underrun_threshold = pcm->period_frames;

	return 0;

fail:
	alsa_pcm_close(pcm);
	if (msg != NULL)
		*msg = strdup(buf);
	free(tmp);
	return err;
}

void alsa_pcm_close(struct alsa_pcm *pcm) {
	if (pcm->pcm != NULL)
		snd_pcm_close(pcm->pcm);
	pcm->pcm = NULL;
}

int alsa_pcm_write(
		struct alsa_pcm *pcm,
		ffb_t *buffer,
		bool drain,
		unsigned int verbose) {

	snd_pcm_sframes_t avail = 0;
	snd_pcm_sframes_t delay = 0;
	snd_pcm_sframes_t ret;

	pcm->underrun = false;
	if ((ret = snd_pcm_avail_delay(pcm->pcm, &avail, &delay)) < 0) {
		if (ret == -EPIPE) {
			warn("ALSA playback PCM underrun");
			pcm->underrun = true;
			snd_pcm_prepare(pcm->pcm);
			avail = pcm->buffer_frames;
			delay = 0;
		}
		else {
			error("ALSA playback PCM error: %s", snd_strerror(ret));
			return -1;
		}
	}

	snd_pcm_sframes_t frames = ffb_len_out(buffer) / pcm->channels;
	snd_pcm_sframes_t written_frames = 0;
	snd_pcm_uframes_t hw_avail = pcm->buffer_frames - avail;

	/* If not draining, write only as many frames as possible without
	 * blocking. If necessary insert silence frames to prevent underrun. */
	if (!drain) {
		if (frames > avail)
			frames = avail;
		else if (hw_avail + frames < pcm->underrun_threshold &&
					snd_pcm_state(pcm->pcm) == SND_PCM_STATE_RUNNING) {
			/* Pad the buffer with enough silence to restore it to the underrun
			 * threshold. */
			const size_t padding_frames = pcm->underrun_threshold - (hw_avail + frames);
			const size_t padding_samples = padding_frames * pcm->channels;
			if (verbose >= 3)
				info("Underrun imminent: inserting %zu silence frames", padding_frames);
			snd_pcm_format_set_silence(pcm->format, buffer->tail, padding_samples);
			ffb_seek(buffer, padding_samples);
			frames = ffb_len_out(buffer) / pcm->channels;
			/* Flag an underrun to indicate that we have caused a discontinuity
			 * in the input stream. */
			pcm->underrun = true;
		}
	}

	while (frames > 0) {
		ret = snd_pcm_writei(pcm->pcm, buffer->data, frames);
		if (ret < 0)
			switch (-ret) {
			case EINTR:
				continue;
			case EPIPE:
				warn("ALSA playback PCM underrun");
				pcm->underrun = true;
				snd_pcm_prepare(pcm->pcm);
				continue;
			default:
				error("ALSA playback PCM write error: %s", snd_strerror(ret));
				return -1;
			}
		else {
			written_frames += ret;
			frames -= ret;
		}
	}

	if (drain) {
		snd_pcm_drain(pcm->pcm);
		pcm->delay = 0;
		pcm->hw_avail = 0;
		ffb_rewind(buffer);
		return 0;
	}

	pcm->delay = delay + written_frames;
	pcm->hw_avail = hw_avail + written_frames;

	/* Move leftovers to the beginning and reposition tail. */
	if (written_frames > 0)
		ffb_shift(buffer, written_frames * pcm->channels);

	return 0;
}

void alsa_pcm_dump(const struct alsa_pcm *pcm, FILE *fp) {
	snd_output_t *out;
	snd_output_stdio_attach(&out, fp, 0);
	snd_pcm_dump(pcm->pcm, out);
	snd_output_close(out);
}
