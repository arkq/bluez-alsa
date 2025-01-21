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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shared/log.h"

static int alsa_pcm_set_hw_params(
		struct alsa_pcm *pcm,
		snd_pcm_format_t format,
		unsigned int channels,
		unsigned int rate,
		unsigned int *buffer_time,
		unsigned int *period_time,
		char **msg) {

	const snd_pcm_access_t access = SND_PCM_ACCESS_RW_INTERLEAVED;
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

	if ((err = snd_pcm_hw_params_set_format(snd_pcm, params, format)) != 0) {
		snprintf(buf, sizeof(buf), "Set format: %s: %s", snd_strerror(err), snd_pcm_format_name(format));
		goto fail;
	}

	if ((err = snd_pcm_hw_params_set_channels(snd_pcm, params, channels)) != 0) {
		snprintf(buf, sizeof(buf), "Set channels: %s: %d", snd_strerror(err), channels);
		goto fail;
	}

	if ((err = snd_pcm_hw_params_set_rate(snd_pcm, params, rate, 0)) != 0) {
		snprintf(buf, sizeof(buf), "Set sample rate: %s: %d", snd_strerror(err), rate);
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
	if (msg != NULL)
		*msg = strdup(buf);
	return err;
}

static int alsa_pcm_set_sw_params(
		struct alsa_pcm *pcm,
		snd_pcm_uframes_t buffer_size,
		snd_pcm_uframes_t period_size,
		char **msg) {

	snd_pcm_t *snd_pcm = pcm->pcm;
	snd_pcm_sw_params_t *params;
	char buf[256];
	int err;

	snd_pcm_sw_params_alloca(&params);

	if ((err = snd_pcm_sw_params_current(snd_pcm, params)) != 0) {
		snprintf(buf, sizeof(buf), "Get current params: %s", snd_strerror(err));
		goto fail;
	}

	/* Start the transfer when the buffer is half full - this allows
	 * spare capacity to accommodate bursts and short breaks in the
	 * Bluetooth stream. */
	snd_pcm_uframes_t threshold = pcm->start_threshold = buffer_size / 2;
	if ((err = snd_pcm_sw_params_set_start_threshold(snd_pcm, params, threshold)) != 0) {
		snprintf(buf, sizeof(buf), "Set start threshold: %s: %lu", snd_strerror(err), threshold);
		goto fail;
	}

	/* Allow the transfer when at least period_size samples can be processed. */
	if ((err = snd_pcm_sw_params_set_avail_min(snd_pcm, params, period_size)) != 0) {
		snprintf(buf, sizeof(buf), "Set avail min: %s: %lu", snd_strerror(err), period_size);
		goto fail;
	}

	if ((err = snd_pcm_sw_params(snd_pcm, params)) != 0) {
		snprintf(buf, sizeof(buf), "%s", snd_strerror(err));
		goto fail;
	}

	return 0;

fail:
	if (msg != NULL)
		*msg = strdup(buf);
	return err;
}

void alsa_pcm_init(struct alsa_pcm *pcm) {
	pcm->pcm = NULL;
}

int alsa_pcm_open(
		struct alsa_pcm *pcm,
		const char *name,
		snd_pcm_format_t format,
		unsigned int channels,
		unsigned int rate,
		unsigned int buffer_time,
		unsigned int period_time,
		int flags,
		char **msg) {

	char *tmp = NULL;
	char buf[256];
	int err;

	if ((err = snd_pcm_open(&pcm->pcm, name, SND_PCM_STREAM_PLAYBACK, flags)) != 0) {
		snprintf(buf, sizeof(buf), "Open PCM: %s", snd_strerror(err));
		goto fail;
	}

	unsigned int actual_buffer_time = buffer_time;
	unsigned int actual_period_time = period_time;
	if ((err = alsa_pcm_set_hw_params(pcm, format, channels, rate,
				&actual_buffer_time, &actual_period_time, &tmp)) != 0) {
		snprintf(buf, sizeof(buf), "Set HW params: %s", tmp);
		goto fail;
	}

	snd_pcm_uframes_t buffer_size, period_size;
	if ((err = snd_pcm_get_params(pcm->pcm, &buffer_size, &period_size)) != 0) {
		snprintf(buf, sizeof(buf), "Get params: %s", snd_strerror(err));
		goto fail;
	}

	if ((err = alsa_pcm_set_sw_params(pcm, buffer_size, period_size, &tmp)) != 0) {
		snprintf(buf, sizeof(buf), "Set SW params: %s", tmp);
		goto fail;
	}

	if ((err = snd_pcm_prepare(pcm->pcm)) != 0) {
		snprintf(buf, sizeof(buf), "Prepare: %s", snd_strerror(err));
		goto fail;
	}

	pcm->format = format;
	pcm->channels = channels;
	pcm->sample_size = snd_pcm_format_size(format, 1);
	pcm->frame_size = snd_pcm_format_size(format, channels);
	pcm->rate = rate;
	pcm->buffer_time = actual_buffer_time;
	pcm->period_time = actual_period_time;
	pcm->buffer_frames = buffer_size;
	pcm->period_frames = period_size;

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

int alsa_pcm_write(struct alsa_pcm *pcm, ffb_t *buffer) {

	size_t samples = ffb_len_out(buffer);
	snd_pcm_sframes_t frames;

	for (;;) {
		frames = samples / pcm->channels;
		if ((frames = snd_pcm_writei(pcm->pcm, buffer->data, frames)) > 0)
			break;
		switch (-frames) {
		case EINTR:
			continue;
		case EPIPE:
			debug("ALSA playback PCM underrun");
			snd_pcm_prepare(pcm->pcm);
			continue;
		default:
			error("ALSA playback PCM write error: %s", snd_strerror(frames));
			return -1;
		}
	}

	/* Move leftovers to the beginning of buffer and reposition tail. */
	ffb_shift(buffer, frames * pcm->channels);
	return 0;
}

void alsa_pcm_dump(const struct alsa_pcm *pcm, FILE *fp) {
	snd_output_t *out;
	snd_output_stdio_attach(&out, fp, 0);
	snd_pcm_dump(pcm->pcm, out);
	snd_output_close(out);
}
