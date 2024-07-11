/*
 * BlueALSA - a2dp-aptx.c
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "a2dp-aptx.h"

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "a2dp.h"
#include "ba-config.h"
#include "ba-transport.h"
#include "ba-transport-pcm.h"
#include "codec-aptx.h"
#include "io.h"
#include "shared/a2dp-codecs.h"
#include "shared/defs.h"
#include "shared/ffb.h"
#include "shared/log.h"
#include "shared/rt.h"

static const struct a2dp_bit_mapping a2dp_aptx_channels[] = {
	{ APTX_CHANNEL_MODE_MONO, 1 },
	{ APTX_CHANNEL_MODE_STEREO, 2 },
	{ 0 }
};

static const struct a2dp_bit_mapping a2dp_aptx_samplings[] = {
	{ APTX_SAMPLING_FREQ_16000, 16000 },
	{ APTX_SAMPLING_FREQ_32000, 32000 },
	{ APTX_SAMPLING_FREQ_44100, 44100 },
	{ APTX_SAMPLING_FREQ_48000, 48000 },
	{ 0 }
};

void *a2dp_aptx_enc_thread(struct ba_transport_pcm *t_pcm) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pcm_thread_cleanup), t_pcm);

	struct ba_transport *t = t_pcm->t;
	struct io_poll io = { .timeout = -1 };

	HANDLE_APTX handle;
	if ((handle = aptxenc_init()) == NULL) {
		error("Couldn't initialize apt-X encoder: %s", strerror(errno));
		goto fail_init;
	}

	ffb_t bt = { 0 };
	ffb_t pcm = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &pcm);
	pthread_cleanup_push(PTHREAD_CLEANUP(aptxenc_destroy), handle);

	const unsigned int channels = t_pcm->channels;
	const size_t aptx_pcm_samples = 4 * channels;
	const size_t aptx_code_len = 2 * sizeof(uint16_t);
	const size_t mtu_write = t->mtu_write;

	if (ffb_init_int16_t(&pcm, aptx_pcm_samples * (mtu_write / aptx_code_len)) == -1 ||
			ffb_init_uint8_t(&bt, mtu_write) == -1) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail_ffb;
	}

	debug_transport_pcm_thread_loop(t_pcm, "START");
	for (ba_transport_pcm_state_set_running(t_pcm);;) {

		switch (io_poll_and_read_pcm(&io, t_pcm, &pcm)) {
		case -1:
			if (errno == ESTALE) {
				ffb_rewind(&pcm);
				continue;
			}
			error("PCM poll and read error: %s", strerror(errno));
			/* fall-through */
		case 0:
			ba_transport_stop_if_no_clients(t);
			continue;
		}

		const int16_t *input = pcm.data;
		const size_t samples = ffb_len_out(&pcm);
		size_t input_samples = samples;

		/* encode and transfer obtained data */
		while (input_samples >= aptx_pcm_samples) {

			size_t output_len = ffb_len_in(&bt);
			size_t pcm_samples = 0;

			/* Generate as many apt-X frames as possible to fill the output buffer
			 * without overflowing it. The size of the output buffer is based on
			 * the socket MTU, so such a transfer should be most efficient. */
			while (input_samples >= aptx_pcm_samples && output_len >= aptx_code_len) {

				size_t encoded = output_len;
				ssize_t len;

				if ((len = aptxenc_encode(handle, input, input_samples, bt.tail, &encoded)) <= 0) {
					error("Apt-X encoding error: %s", strerror(errno));
					break;
				}

				input += len;
				input_samples -= len;
				ffb_seek(&bt, encoded);
				output_len -= encoded;
				pcm_samples += len;

			}

			ssize_t len = ffb_blen_out(&bt);
			if ((len = io_bt_write(t_pcm, bt.data, len)) <= 0) {
				if (len == -1)
					error("BT write error: %s", strerror(errno));
				goto fail;
			}

			/* keep data transfer at a constant bit rate */
			asrsync_sync(&io.asrs, pcm_samples / channels);

			/* update busy delay (encoding overhead) */
			t_pcm->delay = asrsync_get_busy_usec(&io.asrs) / 100;

			/* reinitialize output buffer */
			ffb_rewind(&bt);

		}

		/* If the input buffer was not consumed (due to codesize limit), we
		 * have to append new data to the existing one. Since we do not use
		 * ring buffer, we will simply move unprocessed data to the front
		 * of our linear buffer. */
		ffb_shift(&pcm, samples - input_samples);

	}

fail:
	debug_transport_pcm_thread_loop(t_pcm, "EXIT");
fail_ffb:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
fail_init:
	pthread_cleanup_pop(1);
	return NULL;
}

#if HAVE_APTX_DECODE
__attribute__ ((weak))
void *a2dp_aptx_dec_thread(struct ba_transport_pcm *t_pcm) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pcm_thread_cleanup), t_pcm);

	struct ba_transport *t = t_pcm->t;
	struct io_poll io = { .timeout = -1 };

	HANDLE_APTX handle;
	if ((handle = aptxdec_init()) == NULL) {
		error("Couldn't initialize apt-X decoder: %s", strerror(errno));
		goto fail_init;
	}

	ffb_t bt = { 0 };
	ffb_t pcm = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &pcm);
	pthread_cleanup_push(PTHREAD_CLEANUP(aptxdec_destroy), handle);

	/* Note, that we are allocating space for one extra output packed, which is
	 * required by the aptx_decode_sync() function of libopenaptx library. */
	if (ffb_init_int16_t(&pcm, (t->mtu_read / 4 + 1) * 8) == -1 ||
			ffb_init_uint8_t(&bt, t->mtu_read) == -1) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail_ffb;
	}

	debug_transport_pcm_thread_loop(t_pcm, "START");
	for (ba_transport_pcm_state_set_running(t_pcm);;) {

		ssize_t len;
		ffb_rewind(&bt);
		if ((len = io_poll_and_read_bt(&io, t_pcm, &bt)) <= 0) {
			if (len == -1)
				error("BT poll and read error: %s", strerror(errno));
			goto fail;
		}

		if (!ba_transport_pcm_is_active(t_pcm))
			continue;

		uint8_t *input = bt.data;
		size_t input_len = len;

		ffb_rewind(&pcm);
		while (input_len >= 4) {

			size_t decoded = ffb_len_in(&pcm);
			if ((len = aptxdec_decode(handle, input, input_len, pcm.tail, &decoded)) <= 0) {
				error("Apt-X decoding error: %s", strerror(errno));
				continue;
			}

			input += len;
			input_len -= len;
			ffb_seek(&pcm, decoded);

		}

		const size_t samples = ffb_len_out(&pcm);
		io_pcm_scale(t_pcm, pcm.data, samples);
		if (io_pcm_write(t_pcm, pcm.data, samples) == -1)
			error("PCM write error: %s", strerror(errno));

	}

fail:
	debug_transport_pcm_thread_loop(t_pcm, "EXIT");
fail_ffb:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
fail_init:
	pthread_cleanup_pop(1);
	return NULL;
}
#endif

static int a2dp_aptx_configuration_select(
		const struct a2dp_sep *sep,
		void *capabilities) {

	a2dp_aptx_t *caps = capabilities;
	const a2dp_aptx_t saved = *caps;

	/* narrow capabilities to values supported by BlueALSA */
	if (a2dp_filter_capabilities(sep, &sep->capabilities,
				caps, sizeof(*caps)) != 0)
		return -1;

	unsigned int sampling_freq = 0;
	if (a2dp_bit_mapping_foreach(a2dp_aptx_samplings, caps->sampling_freq,
				a2dp_foreach_get_best_sampling_freq, &sampling_freq) != -1)
		caps->sampling_freq = sampling_freq;
	else {
		error("apt-X: No supported sampling frequencies: %#x", saved.sampling_freq);
		return errno = ENOTSUP, -1;
	}

	unsigned int channel_mode = 0;
	if (a2dp_bit_mapping_foreach(a2dp_aptx_channels, caps->channel_mode,
				a2dp_foreach_get_best_channel_mode, &channel_mode) != -1)
		caps->channel_mode = channel_mode;
	else {
		error("apt-X: No supported channel modes: %#x", saved.channel_mode);
		return errno = ENOTSUP, -1;
	}

	return 0;
}

static int a2dp_aptx_configuration_check(
		const struct a2dp_sep *sep,
		const void *configuration) {

	const a2dp_aptx_t *conf = configuration;
	a2dp_aptx_t conf_v = *conf;

	/* validate configuration against BlueALSA capabilities */
	if (a2dp_filter_capabilities(sep, &sep->capabilities,
				&conf_v, sizeof(conf_v)) != 0)
		return A2DP_CHECK_ERR_SIZE;

	if (a2dp_bit_mapping_lookup(a2dp_aptx_samplings, conf_v.sampling_freq) == 0) {
		debug("apt-X: Invalid sampling frequency: %#x", conf->sampling_freq);
		return A2DP_CHECK_ERR_SAMPLING;
	}

	if (a2dp_bit_mapping_lookup(a2dp_aptx_channels, conf_v.channel_mode) == 0) {
		debug("apt-X: Invalid channel mode: %#x", conf->channel_mode);
		return A2DP_CHECK_ERR_CHANNEL_MODE;
	}

	return A2DP_CHECK_OK;
}

static int a2dp_aptx_transport_init(struct ba_transport *t) {

	unsigned int channels;
	if ((channels = a2dp_bit_mapping_lookup(a2dp_aptx_channels,
					t->a2dp.configuration.aptx.channel_mode)) == 0)
		return -1;

	unsigned int sampling;
	if ((sampling = a2dp_bit_mapping_lookup(a2dp_aptx_samplings,
					t->a2dp.configuration.aptx.sampling_freq)) == 0)
		return -1;

	t->a2dp.pcm.format = BA_TRANSPORT_PCM_FORMAT_S16_2LE;
	t->a2dp.pcm.channels = channels;
	t->a2dp.pcm.sampling = sampling;

	return 0;
}

static int a2dp_aptx_source_init(struct a2dp_sep *sep) {
	if (config.a2dp.force_mono)
		warn("apt-X: Mono channel mode not supported");
	if (config.a2dp.force_44100)
		sep->capabilities.aptx.sampling_freq = APTX_SAMPLING_FREQ_44100;
	return 0;
}

static int a2dp_aptx_source_transport_start(struct ba_transport *t) {
	return ba_transport_pcm_start(&t->a2dp.pcm, a2dp_aptx_enc_thread, "ba-a2dp-aptx");
}

struct a2dp_sep a2dp_aptx_source = {
	.type = A2DP_SOURCE,
	.codec_id = A2DP_CODEC_VENDOR_ID(APTX_VENDOR_ID, APTX_CODEC_ID),
	.synopsis = "A2DP Source (apt-X)",
	.capabilities.aptx = {
		.info = A2DP_VENDOR_INFO_INIT(APTX_VENDOR_ID, APTX_CODEC_ID),
		/* NOTE: Used apt-X library does not support
		 *       single channel (mono) mode. */
		.channel_mode =
			APTX_CHANNEL_MODE_STEREO,
		.sampling_freq =
			APTX_SAMPLING_FREQ_16000 |
			APTX_SAMPLING_FREQ_32000 |
			APTX_SAMPLING_FREQ_44100 |
			APTX_SAMPLING_FREQ_48000,
	},
	.capabilities_size = sizeof(a2dp_aptx_t),
	.init = a2dp_aptx_source_init,
	.configuration_select = a2dp_aptx_configuration_select,
	.configuration_check = a2dp_aptx_configuration_check,
	.transport_init = a2dp_aptx_transport_init,
	.transport_start = a2dp_aptx_source_transport_start,
};

#if HAVE_APTX_DECODE

static int a2dp_aptx_sink_transport_start(struct ba_transport *t) {
	return ba_transport_pcm_start(&t->a2dp.pcm, a2dp_aptx_dec_thread, "ba-a2dp-aptx");
}

struct a2dp_sep a2dp_aptx_sink = {
	.type = A2DP_SINK,
	.codec_id = A2DP_CODEC_VENDOR_ID(APTX_VENDOR_ID, APTX_CODEC_ID),
	.synopsis = "A2DP Sink (apt-X)",
	.capabilities.aptx = {
		.info = A2DP_VENDOR_INFO_INIT(APTX_VENDOR_ID, APTX_CODEC_ID),
		/* NOTE: Used apt-X library does not support
		 *       single channel (mono) mode. */
		.channel_mode =
			APTX_CHANNEL_MODE_STEREO,
		.sampling_freq =
			APTX_SAMPLING_FREQ_16000 |
			APTX_SAMPLING_FREQ_32000 |
			APTX_SAMPLING_FREQ_44100 |
			APTX_SAMPLING_FREQ_48000,
	},
	.capabilities_size = sizeof(a2dp_aptx_t),
	.configuration_select = a2dp_aptx_configuration_select,
	.configuration_check = a2dp_aptx_configuration_check,
	.transport_init = a2dp_aptx_transport_init,
	.transport_start = a2dp_aptx_sink_transport_start,
};

#endif
