/*
 * BlueALSA - a2dp-ldac.c
 * Copyright (c) 2016-2023 Arkadiusz Bokowy
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

#include <glib.h>

#include <ldacBT.h>
#include <ldacBT_abr.h>

#include "a2dp.h"
#include "bluealsa-config.h"
#include "io.h"
#include "rtp.h"
#include "utils.h"
#include "shared/a2dp-codecs.h"
#include "shared/defs.h"
#include "shared/ffb.h"
#include "shared/log.h"
#include "shared/rt.h"

static const struct a2dp_channel_mode a2dp_ldac_channels[] = {
	{ A2DP_CHM_MONO, 1, LDAC_CHANNEL_MODE_MONO },
	{ A2DP_CHM_DUAL_CHANNEL, 2, LDAC_CHANNEL_MODE_DUAL },
	{ A2DP_CHM_STEREO, 2, LDAC_CHANNEL_MODE_STEREO },
};

static const struct a2dp_sampling_freq a2dp_ldac_samplings[] = {
	{ 44100, LDAC_SAMPLING_FREQ_44100 },
	{ 48000, LDAC_SAMPLING_FREQ_48000 },
	{ 88200, LDAC_SAMPLING_FREQ_88200 },
	{ 96000, LDAC_SAMPLING_FREQ_96000 },
};

struct a2dp_codec a2dp_ldac_sink = {
	.dir = A2DP_SINK,
	.codec_id = A2DP_CODEC_VENDOR_LDAC,
	.capabilities.ldac = {
		.info = A2DP_SET_VENDOR_ID_CODEC_ID(LDAC_VENDOR_ID, LDAC_CODEC_ID),
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
	.channels[0] = a2dp_ldac_channels,
	.channels_size[0] = ARRAYSIZE(a2dp_ldac_channels),
	.samplings[0] = a2dp_ldac_samplings,
	.samplings_size[0] = ARRAYSIZE(a2dp_ldac_samplings),
};

struct a2dp_codec a2dp_ldac_source = {
	.dir = A2DP_SOURCE,
	.codec_id = A2DP_CODEC_VENDOR_LDAC,
	.capabilities.ldac = {
		.info = A2DP_SET_VENDOR_ID_CODEC_ID(LDAC_VENDOR_ID, LDAC_CODEC_ID),
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
	.channels[0] = a2dp_ldac_channels,
	.channels_size[0] = ARRAYSIZE(a2dp_ldac_channels),
	.samplings[0] = a2dp_ldac_samplings,
	.samplings_size[0] = ARRAYSIZE(a2dp_ldac_samplings),
};

void a2dp_ldac_init(void) {
}

void a2dp_ldac_transport_init(struct ba_transport *t) {

	const struct a2dp_codec *codec = t->a2dp.codec;

	/* LDAC library internally for encoding uses 31-bit integers or
	 * floats, so the best choice for PCM sample is signed 32-bit. */
	t->a2dp.pcm.format = BA_TRANSPORT_PCM_FORMAT_S32_4LE;

	t->a2dp.pcm.channels = a2dp_codec_lookup_channels(codec,
			t->a2dp.configuration.ldac.channel_mode, false);
	t->a2dp.pcm.sampling = a2dp_codec_lookup_frequency(codec,
			t->a2dp.configuration.ldac.frequency, false);

}

void *a2dp_ldac_enc_thread(struct ba_transport_thread *th) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_thread_cleanup), th);

	struct ba_transport *t = th->t;
	struct ba_transport_pcm *t_pcm = th->pcm;
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

	debug_transport_thread_loop(th, "START");
	for (ba_transport_thread_state_set_running(th);;) {

		ssize_t samples;
		if ((samples = io_poll_and_read_pcm(&io, t_pcm,
						pcm.tail, ffb_len_in(&pcm))) <= 0) {
			if (samples == -1)
				error("PCM poll and read error: %s", strerror(errno));
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
				if ((len = io_bt_write(th, bt.data, len)) <= 0) {
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
	debug_transport_thread_loop(th, "EXIT");
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
void *a2dp_ldac_dec_thread(struct ba_transport_thread *th) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_thread_cleanup), th);

	struct ba_transport *t = th->t;
	struct ba_transport_pcm *t_pcm = th->pcm;
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

	debug_transport_thread_loop(th, "START");
	for (ba_transport_thread_state_set_running(th);;) {

		ssize_t len = ffb_blen_in(&bt);
		if ((len = io_poll_and_read_bt(&io, th, bt.data, len)) <= 0) {
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
	debug_transport_thread_loop(th, "EXIT");
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

int a2dp_ldac_transport_start(struct ba_transport *t) {

	if (t->profile & BA_TRANSPORT_PROFILE_A2DP_SOURCE)
		return ba_transport_thread_create(&t->thread_enc, a2dp_ldac_enc_thread, "ba-a2dp-ldac", true);

#if HAVE_LDAC_DECODE
	if (t->profile & BA_TRANSPORT_PROFILE_A2DP_SINK)
		return ba_transport_thread_create(&t->thread_dec, a2dp_ldac_dec_thread, "ba-a2dp-ldac", true);
#endif

	g_assert_not_reached();
	return -1;
}
