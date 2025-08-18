/*
 * BlueALSA - a2dp-ldac.c
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "a2dp-ldac.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <ldacBT.h>
#include <ldacBT_abr.h>

#include "a2dp.h"
#include "ba-transport.h"
#include "ba-transport-pcm.h"
#include "ba-config.h"
#include "bluealsa-dbus.h"
#include "io.h"
#include "rtp.h"
#include "utils.h"
#include "shared/a2dp-codecs.h"
#include "shared/defs.h"
#include "shared/ffb.h"
#include "shared/log.h"
#include "shared/rt.h"

static const struct a2dp_bit_mapping a2dp_ldac_channels[] = {
	{ LDAC_CHANNEL_MODE_MONO, .ch = { 1, a2dp_channel_map_mono } },
	{ LDAC_CHANNEL_MODE_DUAL, .ch = { 2, a2dp_channel_map_stereo } },
	{ LDAC_CHANNEL_MODE_STEREO, .ch = { 2, a2dp_channel_map_stereo } },
	{ 0 }
};

static const struct a2dp_bit_mapping a2dp_ldac_rates[] = {
	{ LDAC_SAMPLING_FREQ_44100, { 44100 } },
	{ LDAC_SAMPLING_FREQ_48000, { 48000 } },
	{ LDAC_SAMPLING_FREQ_88200, { 88200 } },
	{ LDAC_SAMPLING_FREQ_96000, { 96000 } },
	{ LDAC_SAMPLING_FREQ_176400, { 176400 } },
	{ LDAC_SAMPLING_FREQ_192000, { 192000 } },
	{ 0 }
};

static void a2dp_ldac_caps_intersect(
		void *capabilities,
		const void *mask) {
	a2dp_caps_bitwise_intersect(capabilities, mask, sizeof(a2dp_ldac_t));
}

static int a2dp_ldac_caps_foreach_channel_mode(
		const void *capabilities,
		enum a2dp_stream stream,
		a2dp_bit_mapping_foreach_func func,
		void *userdata) {
	const a2dp_ldac_t *caps = capabilities;
	if (stream == A2DP_MAIN)
		return a2dp_bit_mapping_foreach(a2dp_ldac_channels, caps->channel_mode, func, userdata);
	return -1;
}

static int a2dp_ldac_caps_foreach_sample_rate(
		const void *capabilities,
		enum a2dp_stream stream,
		a2dp_bit_mapping_foreach_func func,
		void *userdata) {
	const a2dp_ldac_t *caps = capabilities;
	if (stream == A2DP_MAIN)
		return a2dp_bit_mapping_foreach(a2dp_ldac_rates, caps->sampling_freq, func, userdata);
	return -1;
}

static void a2dp_ldac_caps_select_channel_mode(
		void *capabilities,
		enum a2dp_stream stream,
		unsigned int channels) {
	a2dp_ldac_t *caps = capabilities;
	if (stream == A2DP_MAIN)
		caps->channel_mode = a2dp_bit_mapping_lookup_value(a2dp_ldac_channels,
				caps->channel_mode, channels);
}

static void a2dp_ldac_caps_select_sample_rate(
		void *capabilities,
		enum a2dp_stream stream,
		unsigned int rate) {
	a2dp_ldac_t *caps = capabilities;
	if (stream == A2DP_MAIN)
		caps->sampling_freq = a2dp_bit_mapping_lookup_value(a2dp_ldac_rates,
				caps->sampling_freq, rate);
}

static struct a2dp_caps_helpers a2dp_ldac_caps_helpers = {
	.intersect = a2dp_ldac_caps_intersect,
	.has_stream = a2dp_caps_has_main_stream_only,
	.foreach_channel_mode = a2dp_ldac_caps_foreach_channel_mode,
	.foreach_sample_rate = a2dp_ldac_caps_foreach_sample_rate,
	.select_channel_mode = a2dp_ldac_caps_select_channel_mode,
	.select_sample_rate = a2dp_ldac_caps_select_sample_rate,
};

void *a2dp_ldac_enc_thread(struct ba_transport_pcm *t_pcm) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pcm_thread_cleanup), t_pcm);

	struct ba_transport *t = t_pcm->t;
	struct io_poll io = { .timeout = -1 };

	HANDLE_LDAC_BT handle;
	if ((handle = ldacBT_get_handle()) == NULL) {
		error("Couldn't get LDAC handle: %s", strerror(errno));
		goto fail_open_ldac;
	}

	pthread_cleanup_push(PTHREAD_CLEANUP(ldacBT_free_handle), handle);

	HANDLE_LDAC_ABR handle_abr;
	if ((handle_abr = ldac_ABR_get_handle()) == NULL) {
		error("Couldn't get LDAC ABR handle: %s", strerror(errno));
		goto fail_open_ldac_abr;
	}

	pthread_cleanup_push(PTHREAD_CLEANUP(ldac_ABR_free_handle), handle_abr);

	const a2dp_ldac_t *configuration = &t->media.configuration.ldac;
	const size_t sample_size = BA_TRANSPORT_PCM_FORMAT_BYTES(t_pcm->format);
	const unsigned int channels = t_pcm->channels;
	const unsigned int rate = t_pcm->rate;
	const size_t ldac_frame_pcm_samples = LDACBT_ENC_LSU * channels;

	if (ldacBT_init_handle_encode(handle, t->mtu_write, config.ldac_eqmid,
				configuration->channel_mode, LDACBT_SMPL_FMT_S32, rate) == -1) {
		error("Couldn't initialize LDAC encoder: %s", ldacBT_strerror(ldacBT_get_error_code(handle)));
		goto fail_init;
	}

	if (ldac_ABR_Init(handle_abr, 1000 * ldac_frame_pcm_samples / channels / rate) == -1) {
		error("Couldn't initialize LDAC ABR");
		goto fail_init;
	}
	if (ldac_ABR_set_thresholds(handle_abr, 6, 4, 2) == -1) {
		error("Couldn't set LDAC ABR thresholds");
		goto fail_init;
	}

	ffb_t bt = { 0 };
	ffb_t pcm = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &pcm);

	if (ffb_init_int32_t(&pcm, ldac_frame_pcm_samples) == -1 ||
			ffb_init_uint8_t(&bt, t->mtu_write) == -1) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail_ffb;
	}

	const unsigned int ldac_delay_pcm_frames = 128;
	/* Get the total delay introduced by the codec. */
	t_pcm->codec_delay_dms = ldac_delay_pcm_frames * 10000 / rate;
	ba_transport_pcm_delay_sync(t_pcm, BA_DBUS_PCM_UPDATE_DELAY);

	rtp_header_t *rtp_header;
	rtp_media_header_t *rtp_media_header;
	/* initialize RTP headers and get anchor for payload */
	uint8_t *rtp_payload = rtp_a2dp_init(bt.data, &rtp_header,
			(void **)&rtp_media_header, sizeof(*rtp_media_header));

	struct rtp_state rtp = { .synced = false };
	/* RTP clock frequency equal to PCM sample rate */
	rtp_state_init(&rtp, rate, rate);

	debug_transport_pcm_thread_loop(t_pcm, "START");
	for (ba_transport_pcm_state_set_running(t_pcm);;) {

		switch (io_poll_and_read_pcm(&io, t_pcm, &pcm)) {
		case -1:
			if (errno == ESTALE) {
				int tmp;
				/* flush encoder internal buffers */
				ldacBT_encode(handle, NULL, &tmp, rtp_payload, &tmp, &tmp);
				continue;
			}
			error("PCM poll and read error: %s", strerror(errno));
			/* fall-through */
		case 0:
			ba_transport_stop_if_no_clients(t);
			continue;
		}

		int16_t *input = pcm.data;
		size_t samples = ffb_len_out(&pcm);
		size_t input_len = samples;

		/* Encode and transfer obtained data. */
		while (input_len >= ldac_frame_pcm_samples) {

			/* anchor for RTP payload */
			bt.tail = rtp_payload;

			int used;
			int encoded;
			int frames;

			if (ldacBT_encode(handle, input, &used, bt.tail, &encoded, &frames) != 0) {
				error("LDAC encoding error: %s", ldacBT_strerror(ldacBT_get_error_code(handle)));
				break;
			}

			rtp_media_header->frame_count = frames;

			size_t pcm_samples = used / sample_size;
			input += pcm_samples;
			input_len -= pcm_samples;
			ffb_seek(&bt, encoded);

			if (encoded > 0) {

				rtp_state_new_frame(&rtp, rtp_header);

				/* Try to get the number of bytes queued in the
				 * socket output buffer. */
				int queued_bytes = 0;
				if (ioctl(t->bt_fd, TIOCOUTQ, &queued_bytes) != -1)
					queued_bytes = abs(t->media.bt_fd_coutq_init - queued_bytes);

				errno = 0;

				ssize_t len = ffb_blen_out(&bt);
				if ((len = io_bt_write(t_pcm, bt.data, len)) <= 0) {
					if (len == -1)
						error("BT write error: %s", strerror(errno));
					goto fail;
				}

				if (!io.initiated) {
					/* Get the delay due to codec processing. */
					t_pcm->processing_delay_dms = asrsync_get_dms_since_last_sync(&io.asrs);
					ba_transport_pcm_delay_sync(t_pcm, BA_DBUS_PCM_UPDATE_DELAY);
					io.initiated = true;
				}

				if (errno == EAGAIN)
					/* The io_bt_write() call was blocking due to not enough
					 * space in the BT socket. Set the queued_bytes to some
					 * arbitrary big value. */
					queued_bytes = 1024 * 16;

				if (config.ldac_abr)
					ldac_ABR_Proc(handle, handle_abr, queued_bytes / t->mtu_write, 1);

			}

			const size_t pcm_frames = pcm_samples / channels;
			/* Keep data transfer at a constant bit rate. */
			asrsync_sync(&io.asrs, pcm_frames);
			/* move forward RTP timestamp clock */
			rtp_state_update(&rtp, pcm_frames);

		}

		/* If the input buffer was not consumed (due to codesize limit), we
		 * have to append new data to the existing one. Since we do not use
		 * ring buffer, we will simply move unprocessed data to the front
		 * of our linear buffer. */
		ffb_shift(&pcm, samples - input_len);

	}

fail:
	debug_transport_pcm_thread_loop(t_pcm, "EXIT");
fail_ffb:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
fail_init:
	pthread_cleanup_pop(1);
fail_open_ldac_abr:
	pthread_cleanup_pop(1);
fail_open_ldac:
	pthread_cleanup_pop(1);
	return NULL;
}

#if HAVE_LDAC_DECODE
__attribute__ ((weak))
void *a2dp_ldac_dec_thread(struct ba_transport_pcm *t_pcm) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pcm_thread_cleanup), t_pcm);

	struct ba_transport *t = t_pcm->t;
	struct io_poll io = { .timeout = -1 };

	HANDLE_LDAC_BT handle;
	if ((handle = ldacBT_get_handle()) == NULL) {
		error("Couldn't get LDAC handle: %s", strerror(errno));
		goto fail_open;
	}

	pthread_cleanup_push(PTHREAD_CLEANUP(ldacBT_free_handle), handle);

	const a2dp_ldac_t *configuration = &t->media.configuration.ldac;
	const size_t sample_size = BA_TRANSPORT_PCM_FORMAT_BYTES(t_pcm->format);
	const unsigned int channels = t_pcm->channels;
	const unsigned int rate = t_pcm->rate;

	if (ldacBT_init_handle_decode(handle, configuration->channel_mode, rate, 0, 0, 0) == -1) {
		error("Couldn't initialize LDAC decoder: %s", ldacBT_strerror(ldacBT_get_error_code(handle)));
		goto fail_init;
	}

	ffb_t bt = { 0 };
	ffb_t pcm = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &pcm);

	if (ffb_init_int32_t(&pcm, LDACBT_MAX_LSU * channels) == -1 ||
			ffb_init_uint8_t(&bt, t->mtu_read) == -1) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail_ffb;
	}

	struct rtp_state rtp = { .synced = false };
	/* RTP clock frequency equal to PCM sample rate */
	rtp_state_init(&rtp, rate, rate);

	debug_transport_pcm_thread_loop(t_pcm, "START");
	for (ba_transport_pcm_state_set_running(t_pcm);;) {

		ssize_t len;
		ffb_rewind(&bt);
		if ((len = io_poll_and_read_bt(&io, t_pcm, &bt)) <= 0) {
			if (len == -1)
				error("BT poll and read error: %s", strerror(errno));
			goto fail;
		}

		const rtp_header_t *rtp_header = bt.data;
		const rtp_media_header_t *rtp_media_header;
		if ((rtp_media_header = rtp_a2dp_get_payload(rtp_header)) == NULL)
			continue;

		int missing_rtp_frames = 0;
		rtp_state_sync_stream(&rtp, rtp_header, &missing_rtp_frames, NULL);

		if (!ba_transport_pcm_is_active(t_pcm)) {
			rtp.synced = false;
			continue;
		}

		const uint8_t *rtp_payload = (uint8_t *)(rtp_media_header + 1);
		size_t rtp_payload_len = len - (rtp_payload - (uint8_t *)bt.data);

		size_t frames = rtp_media_header->frame_count;
		while (frames--) {

			int used;
			int decoded;

			if (ldacBT_decode(handle, (void *)rtp_payload, pcm.data,
						LDACBT_SMPL_FMT_S32, rtp_payload_len, &used, &decoded) != 0) {
				error("LDAC decoding error: %s", ldacBT_strerror(ldacBT_get_error_code(handle)));
				break;
			}

			rtp_payload += used;
			rtp_payload_len -= used;

			const size_t samples = decoded / sample_size;
			io_pcm_scale(t_pcm, pcm.data, samples);
			if (io_pcm_write(t_pcm, pcm.data, samples) == -1)
				error("PCM write error: %s", strerror(errno));

			/* update local state with decoded PCM frames */
			rtp_state_update(&rtp, samples / channels);

		}

	}

fail:
	debug_transport_pcm_thread_loop(t_pcm, "EXIT");
fail_ffb:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
fail_init:
	pthread_cleanup_pop(1);
fail_open:
	pthread_cleanup_pop(1);
	return NULL;
}
#endif

static int a2dp_ldac_configuration_select(
		const struct a2dp_sep *sep,
		void *capabilities) {

	a2dp_ldac_t *caps = capabilities;
	const a2dp_ldac_t saved = *caps;

	/* Narrow capabilities to values supported by BlueALSA. */
	a2dp_ldac_caps_intersect(caps, &sep->config.capabilities);

	unsigned int sampling_freq = 0;
	if (a2dp_ldac_caps_foreach_sample_rate(caps, A2DP_MAIN,
				a2dp_bit_mapping_foreach_get_best_sample_rate, &sampling_freq) != -1)
		caps->sampling_freq = sampling_freq;
	else {
		error("LDAC: No supported sample rates: %#x", saved.sampling_freq);
		return errno = ENOTSUP, -1;
	}

	unsigned int channel_mode = 0;
	if (a2dp_ldac_caps_foreach_channel_mode(caps, A2DP_MAIN,
				a2dp_bit_mapping_foreach_get_best_channel_mode, &channel_mode) != -1)
		caps->channel_mode = channel_mode;
	else {
		error("LDAC: No supported channel modes: %#x", saved.channel_mode);
		return errno = ENOTSUP, -1;
	}

	return 0;
}

static int a2dp_ldac_configuration_check(
		const struct a2dp_sep *sep,
		const void *configuration) {

	const a2dp_ldac_t *conf = configuration;
	a2dp_ldac_t conf_v = *conf;

	/* Validate configuration against BlueALSA capabilities. */
	a2dp_ldac_caps_intersect(&conf_v, &sep->config.capabilities);

	if (a2dp_bit_mapping_lookup(a2dp_ldac_rates, conf_v.sampling_freq) == -1) {
		debug("LDAC: Invalid sample rate: %#x", conf->sampling_freq);
		return A2DP_CHECK_ERR_RATE;
	}

	if (a2dp_bit_mapping_lookup(a2dp_ldac_channels, conf_v.channel_mode) == -1) {
		debug("LDAC: Invalid channel mode: %#x", conf->channel_mode);
		return A2DP_CHECK_ERR_CHANNEL_MODE;
	}

	return A2DP_CHECK_OK;
}

static int a2dp_ldac_transport_init(struct ba_transport *t) {

	ssize_t channels_i;
	if ((channels_i = a2dp_bit_mapping_lookup(a2dp_ldac_channels,
					t->media.configuration.ldac.channel_mode)) == -1)
		return -1;

	ssize_t rate_i;
	if ((rate_i = a2dp_bit_mapping_lookup(a2dp_ldac_rates,
					t->media.configuration.ldac.sampling_freq)) == -1)
		return -1;

	/* LDAC library internally for encoding uses 31-bit integers or
	 * floats, so the best choice for PCM sample is signed 32-bit. */
	t->media.pcm.format = BA_TRANSPORT_PCM_FORMAT_S32_4LE;
	t->media.pcm.channels = a2dp_ldac_channels[channels_i].value;
	t->media.pcm.rate = a2dp_ldac_rates[rate_i].value;

	memcpy(t->media.pcm.channel_map, a2dp_ldac_channels[channels_i].ch.map,
			t->media.pcm.channels * sizeof(*t->media.pcm.channel_map));

	return 0;
}

static int a2dp_ldac_source_init(struct a2dp_sep *sep) {
	if (config.a2dp.force_mono)
		sep->config.capabilities.ldac.channel_mode = LDAC_CHANNEL_MODE_MONO;
	if (config.a2dp.force_44100)
		sep->config.capabilities.ldac.sampling_freq = LDAC_SAMPLING_FREQ_44100;
	return 0;
}

static int a2dp_ldac_source_transport_start(struct ba_transport *t) {
	return ba_transport_pcm_start(&t->media.pcm, a2dp_ldac_enc_thread, "ba-a2dp-ldac");
}

struct a2dp_sep a2dp_ldac_source = {
	.name = "A2DP Source (LDAC)",
	.config = {
		.type = A2DP_SOURCE,
		.codec_id = A2DP_CODEC_VENDOR_ID(LDAC_VENDOR_ID, LDAC_CODEC_ID),
		.caps_size = sizeof(a2dp_ldac_t),
		.capabilities.ldac = {
			.info = A2DP_VENDOR_INFO_INIT(LDAC_VENDOR_ID, LDAC_CODEC_ID),
			.channel_mode =
				LDAC_CHANNEL_MODE_MONO |
				LDAC_CHANNEL_MODE_DUAL |
				LDAC_CHANNEL_MODE_STEREO,
			/* NOTE: Used LDAC library does not support
			 *       frequencies higher than 96 kHz. */
			.sampling_freq =
				LDAC_SAMPLING_FREQ_44100 |
				LDAC_SAMPLING_FREQ_48000 |
				LDAC_SAMPLING_FREQ_88200 |
				LDAC_SAMPLING_FREQ_96000,
		},
	},
	.init = a2dp_ldac_source_init,
	.configuration_select = a2dp_ldac_configuration_select,
	.configuration_check = a2dp_ldac_configuration_check,
	.transport_init = a2dp_ldac_transport_init,
	.transport_start = a2dp_ldac_source_transport_start,
	.caps_helpers = &a2dp_ldac_caps_helpers,
};

#if HAVE_LDAC_DECODE

static int a2dp_ldac_sink_transport_start(struct ba_transport *t) {
	return ba_transport_pcm_start(&t->media.pcm, a2dp_ldac_dec_thread, "ba-a2dp-ldac");
}

struct a2dp_sep a2dp_ldac_sink = {
	.name = "A2DP Sink (LDAC)",
	.config = {
		.type = A2DP_SINK,
		.codec_id = A2DP_CODEC_VENDOR_ID(LDAC_VENDOR_ID, LDAC_CODEC_ID),
		.caps_size = sizeof(a2dp_ldac_t),
		.capabilities.ldac = {
			.info = A2DP_VENDOR_INFO_INIT(LDAC_VENDOR_ID, LDAC_CODEC_ID),
			.channel_mode =
				LDAC_CHANNEL_MODE_MONO |
				LDAC_CHANNEL_MODE_DUAL |
				LDAC_CHANNEL_MODE_STEREO,
			/* NOTE: Used LDAC library does not support
			 *       frequencies higher than 96 kHz. */
			.sampling_freq =
				LDAC_SAMPLING_FREQ_44100 |
				LDAC_SAMPLING_FREQ_48000 |
				LDAC_SAMPLING_FREQ_88200 |
				LDAC_SAMPLING_FREQ_96000,
		},
	},
	.configuration_select = a2dp_ldac_configuration_select,
	.configuration_check = a2dp_ldac_configuration_check,
	.transport_init = a2dp_ldac_transport_init,
	.transport_start = a2dp_ldac_sink_transport_start,
	.caps_helpers = &a2dp_ldac_caps_helpers,
};

#endif
