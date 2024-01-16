/*
 * BlueALSA - a2dp-ldac.c
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
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
#include "bluealsa-config.h"
#include "io.h"
#include "rtp.h"
#include "utils.h"
#include "shared/a2dp-codecs.h"
#include "shared/defs.h"
#include "shared/ffb.h"
#include "shared/log.h"
#include "shared/rt.h"

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

	const a2dp_ldac_t *configuration = &t->a2dp.configuration.ldac;
	const size_t sample_size = BA_TRANSPORT_PCM_FORMAT_BYTES(t_pcm->format);
	const unsigned int channels = t_pcm->channels;
	const unsigned int samplerate = t_pcm->sampling;
	const size_t ldac_pcm_samples = LDACBT_ENC_LSU * channels;

	if (ldacBT_init_handle_encode(handle, t->mtu_write, config.ldac_eqmid,
				configuration->channel_mode, LDACBT_SMPL_FMT_S32, samplerate) == -1) {
		error("Couldn't initialize LDAC encoder: %s", ldacBT_strerror(ldacBT_get_error_code(handle)));
		goto fail_init;
	}

	if (ldac_ABR_Init(handle_abr, 1000 * ldac_pcm_samples / channels / samplerate) == -1) {
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

	if (ffb_init_int32_t(&pcm, ldac_pcm_samples) == -1 ||
			ffb_init_uint8_t(&bt, t->mtu_write) == -1) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail_ffb;
	}

	rtp_header_t *rtp_header;
	rtp_media_header_t *rtp_media_header;
	/* initialize RTP headers and get anchor for payload */
	uint8_t *rtp_payload = rtp_a2dp_init(bt.data, &rtp_header,
			(void **)&rtp_media_header, sizeof(*rtp_media_header));

	struct rtp_state rtp = { .synced = false };
	/* RTP clock frequency equal to audio samplerate */
	rtp_state_init(&rtp, samplerate, samplerate);

	debug_transport_pcm_thread_loop(t_pcm, "START");
	for (ba_transport_pcm_state_set_running(t_pcm);;) {

		ssize_t samples = ffb_len_in(&pcm);
		switch (samples = io_poll_and_read_pcm(&io, t_pcm, pcm.tail, samples)) {
		case -1:
			if (errno == ESTALE) {
				int tmp;
				/* flush encoder internal buffers */
				ldacBT_encode(handle, NULL, &tmp, rtp_payload, &tmp, &tmp);
				ffb_rewind(&pcm);
				continue;
			}
			error("PCM poll and read error: %s", strerror(errno));
			/* fall-through */
		case 0:
			ba_transport_stop_if_no_clients(t);
			continue;
		}

		ffb_seek(&pcm, samples);
		samples = ffb_len_out(&pcm);

		int16_t *input = pcm.data;
		size_t input_len = samples;

		/* encode and transfer obtained data */
		while (input_len >= ldac_pcm_samples) {

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
					queued_bytes = abs(t->a2dp.bt_fd_coutq_init - queued_bytes);

				errno = 0;

				ssize_t len = ffb_blen_out(&bt);
				if ((len = io_bt_write(t_pcm, bt.data, len)) <= 0) {
					if (len == -1)
						error("BT write error: %s", strerror(errno));
					goto fail;
				}

				if (errno == EAGAIN)
					/* The io_bt_write() call was blocking due to not enough
					 * space in the BT socket. Set the queued_bytes to some
					 * arbitrary big value. */
					queued_bytes = 1024 * 16;

				if (config.ldac_abr)
					ldac_ABR_Proc(handle, handle_abr, queued_bytes / t->mtu_write, 1);

			}

			unsigned int pcm_frames = pcm_samples / channels;
			/* keep data transfer at a constant bit rate */
			asrsync_sync(&io.asrs, pcm_frames);
			/* move forward RTP timestamp clock */
			rtp_state_update(&rtp, pcm_frames);

			/* update busy delay (encoding overhead) */
			t_pcm->delay = asrsync_get_busy_usec(&io.asrs) / 100;

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

	const a2dp_ldac_t *configuration = &t->a2dp.configuration.ldac;
	const size_t sample_size = BA_TRANSPORT_PCM_FORMAT_BYTES(t_pcm->format);
	const unsigned int channels = t_pcm->channels;
	const unsigned int samplerate = t_pcm->sampling;

	if (ldacBT_init_handle_decode(handle, configuration->channel_mode, samplerate, 0, 0, 0) == -1) {
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
	/* RTP clock frequency equal to audio samplerate */
	rtp_state_init(&rtp, samplerate, samplerate);

	debug_transport_pcm_thread_loop(t_pcm, "START");
	for (ba_transport_pcm_state_set_running(t_pcm);;) {

		ssize_t len = ffb_blen_in(&bt);
		if ((len = io_poll_and_read_bt(&io, t_pcm, bt.data, len)) <= 0) {
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
				error("FIFO write error: %s", strerror(errno));

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

static const struct a2dp_channels a2dp_ldac_channels[] = {
	{ 1, LDAC_CHANNEL_MODE_MONO },
	{ 2, LDAC_CHANNEL_MODE_DUAL },
	{ 2, LDAC_CHANNEL_MODE_STEREO },
	{ 0 },
};

static const struct a2dp_sampling a2dp_ldac_samplings[] = {
	{ 44100, LDAC_SAMPLING_FREQ_44100 },
	{ 48000, LDAC_SAMPLING_FREQ_48000 },
	{ 88200, LDAC_SAMPLING_FREQ_88200 },
	{ 96000, LDAC_SAMPLING_FREQ_96000 },
	{ 0 },
};

static int a2dp_ldac_configuration_select(
		const struct a2dp_codec *codec,
		void *capabilities) {

	a2dp_ldac_t *caps = capabilities;
	const a2dp_ldac_t saved = *caps;

	/* narrow capabilities to values supported by BlueALSA */
	if (a2dp_filter_capabilities(codec, &codec->capabilities,
				caps, sizeof(*caps)) != 0)
		return -1;

	const struct a2dp_sampling *sampling;
	if ((sampling = a2dp_sampling_select(a2dp_ldac_samplings, caps->frequency)) != NULL)
		caps->frequency = sampling->value;
	else {
		error("LDAC: No supported sampling frequencies: %#x", saved.frequency);
		return errno = ENOTSUP, -1;
	}

	const struct a2dp_channels *channels;
	if ((channels = a2dp_channels_select(a2dp_ldac_channels, caps->channel_mode)) != NULL)
		caps->channel_mode = channels->value;
	else {
		error("LDAC: No supported channel modes: %#x", saved.channel_mode);
		return errno = ENOTSUP, -1;
	}

	return 0;
}

static int a2dp_ldac_configuration_check(
		const struct a2dp_codec *codec,
		const void *configuration) {

	const a2dp_ldac_t *conf = configuration;
	a2dp_ldac_t conf_v = *conf;

	/* validate configuration against BlueALSA capabilities */
	if (a2dp_filter_capabilities(codec, &codec->capabilities,
				&conf_v, sizeof(conf_v)) != 0)
		return A2DP_CHECK_ERR_SIZE;

	if (a2dp_sampling_lookup(a2dp_ldac_samplings, conf_v.frequency) == NULL) {
		debug("LDAC: Invalid sampling frequency: %#x", conf->frequency);
		return A2DP_CHECK_ERR_SAMPLING;
	}

	if (a2dp_channels_lookup(a2dp_ldac_channels, conf_v.channel_mode) == NULL) {
		debug("LDAC: Invalid channel mode: %#x", conf->channel_mode);
		return A2DP_CHECK_ERR_CHANNEL_MODE;
	}

	return A2DP_CHECK_OK;
}

static int a2dp_ldac_transport_init(struct ba_transport *t) {

	const struct a2dp_channels *channels;
	if ((channels = a2dp_channels_lookup(a2dp_ldac_channels,
					t->a2dp.configuration.ldac.channel_mode)) == NULL)
		return -1;

	const struct a2dp_sampling *sampling;
	if ((sampling = a2dp_sampling_lookup(a2dp_ldac_samplings,
					t->a2dp.configuration.ldac.frequency)) == NULL)
		return -1;

	/* LDAC library internally for encoding uses 31-bit integers or
	 * floats, so the best choice for PCM sample is signed 32-bit. */
	t->a2dp.pcm.format = BA_TRANSPORT_PCM_FORMAT_S32_4LE;
	t->a2dp.pcm.channels = channels->count;
	t->a2dp.pcm.sampling = sampling->frequency;

	return 0;
}

static int a2dp_ldac_source_init(struct a2dp_codec *codec) {
	if (config.a2dp.force_mono)
		codec->capabilities.ldac.channel_mode = LDAC_CHANNEL_MODE_MONO;
	if (config.a2dp.force_44100)
		codec->capabilities.ldac.frequency = LDAC_SAMPLING_FREQ_44100;
	return 0;
}

static int a2dp_ldac_source_transport_start(struct ba_transport *t) {
	return ba_transport_pcm_start(&t->a2dp.pcm, a2dp_ldac_enc_thread, "ba-a2dp-ldac");
}

struct a2dp_codec a2dp_ldac_source = {
	.dir = A2DP_SOURCE,
	.codec_id = A2DP_CODEC_VENDOR_LDAC,
	.synopsis = "A2DP Source (LDAC)",
	.capabilities.ldac = {
		.info = A2DP_VENDOR_INFO_INIT(LDAC_VENDOR_ID, LDAC_CODEC_ID),
		.channel_mode =
			LDAC_CHANNEL_MODE_MONO |
			LDAC_CHANNEL_MODE_DUAL |
			LDAC_CHANNEL_MODE_STEREO,
		/* NOTE: Used LDAC library does not support
		 *       frequencies higher than 96 kHz. */
		.frequency =
			LDAC_SAMPLING_FREQ_44100 |
			LDAC_SAMPLING_FREQ_48000 |
			LDAC_SAMPLING_FREQ_88200 |
			LDAC_SAMPLING_FREQ_96000,
	},
	.capabilities_size = sizeof(a2dp_ldac_t),
	.init = a2dp_ldac_source_init,
	.configuration_select = a2dp_ldac_configuration_select,
	.configuration_check = a2dp_ldac_configuration_check,
	.transport_init = a2dp_ldac_transport_init,
	.transport_start = a2dp_ldac_source_transport_start,
};

#if HAVE_LDAC_DECODE

static int a2dp_ldac_sink_transport_start(struct ba_transport *t) {
	return ba_transport_pcm_start(&t->a2dp.pcm, a2dp_ldac_dec_thread, "ba-a2dp-ldac");
}

struct a2dp_codec a2dp_ldac_sink = {
	.dir = A2DP_SINK,
	.codec_id = A2DP_CODEC_VENDOR_LDAC,
	.synopsis = "A2DP Sink (LDAC)",
	.capabilities.ldac = {
		.info = A2DP_VENDOR_INFO_INIT(LDAC_VENDOR_ID, LDAC_CODEC_ID),
		.channel_mode =
			LDAC_CHANNEL_MODE_MONO |
			LDAC_CHANNEL_MODE_DUAL |
			LDAC_CHANNEL_MODE_STEREO,
		/* NOTE: Used LDAC library does not support
		 *       frequencies higher than 96 kHz. */
		.frequency =
			LDAC_SAMPLING_FREQ_44100 |
			LDAC_SAMPLING_FREQ_48000 |
			LDAC_SAMPLING_FREQ_88200 |
			LDAC_SAMPLING_FREQ_96000,
	},
	.capabilities_size = sizeof(a2dp_ldac_t),
	.configuration_select = a2dp_ldac_configuration_select,
	.configuration_check = a2dp_ldac_configuration_check,
	.transport_init = a2dp_ldac_transport_init,
	.transport_start = a2dp_ldac_sink_transport_start,
};

#endif
