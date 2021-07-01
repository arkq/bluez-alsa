/*
 * BlueALSA - a2dp-audio.c
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "a2dp-audio.h"

#include <ctype.h>
#include <endian.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <glib.h>

#if ENABLE_AAC
# include <fdk-aac/aacdecoder_lib.h>
# include <fdk-aac/aacenc_lib.h>
# define AACENCODER_LIB_VERSION LIB_VERSION( \
		AACENCODER_LIB_VL0, AACENCODER_LIB_VL1, AACENCODER_LIB_VL2)
#endif
#if ENABLE_MP3LAME
# include <lame/lame.h>
#endif
#if ENABLE_LDAC
# include <ldacBT.h>
# include <ldacBT_abr.h>
#endif
#if ENABLE_MPG123
# include <mpg123.h>
#endif
#include <sbc/sbc.h>

#include "a2dp.h"
#include "a2dp-codecs.h"
#include "a2dp-rtp.h"
#include "bluealsa.h"
#if ENABLE_APTX || ENABLE_APTX_HD
# include "codec-aptx.h"
#endif
#include "codec-sbc.h"
#include "io.h"
#include "utils.h"
#include "shared/defs.h"
#include "shared/ffb.h"
#include "shared/log.h"
#include "shared/rt.h"

struct a2dp_io_poll {
	struct io_poll io;
	/* associated transport thread */
	struct ba_transport_thread *th;
	/* history of BT socket COUTQ bytes */
	struct { int v[16]; size_t i; } coutq;
	/* local counter for RTP sequence number */
	uint16_t rtp_seq_number;
};

static enum ba_transport_thread_signal a2dp_io_poll_signal_filter_dec(
		enum ba_transport_thread_signal signal, void *userdata) {
	struct a2dp_io_poll *io = userdata;

	if (signal == BA_TRANSPORT_THREAD_SIGNAL_PCM_CLOSE)
		io->rtp_seq_number = 0;

	return signal;
}

/**
 * Poll and read PCM signal from the transport PCM FIFO.
 *
 * Note:
 * This function temporally re-enables thread cancellation! */
static ssize_t a2dp_poll_and_read_pcm(struct a2dp_io_poll *io,
		struct ba_transport_pcm *pcm, ffb_t *buffer) {

	ssize_t samples;
	if ((samples = io_poll_and_read_pcm(&io->io, pcm,
					buffer->tail, ffb_len_in(buffer))) <= 0)
		return samples;

	/* update PCM buffer */
	ffb_seek(buffer, samples);

	/* return overall number of samples */
	return ffb_len_out(buffer);
}

/**
 * Poll and read BT data from the SEQPACKET socket.
 *
 * Note:
 * This function temporally re-enables thread cancellation! */
static ssize_t a2dp_poll_and_read_bt(struct a2dp_io_poll *io, ffb_t *buffer) {
	return io_poll_and_read_bt(&io->io, io->th, buffer->tail, ffb_blen_in(buffer));
}

/**
 * Write data to the BT SEQPACKET socket.
 *
 * Note:
 * This function may temporally re-enables thread cancellation! */
static ssize_t a2dp_write_bt(struct a2dp_io_poll *io, ffb_t *buffer) {

	struct ba_transport *t = io->th->t;
	struct ba_transport_thread *th = io->th;
	int coutq = 0;
	ssize_t ret;

	/* Try to get the number of bytes queued in the socket output buffer. */
	if (ioctl(t->bt_fd, TIOCOUTQ, &coutq) != -1)
		coutq = abs(t->a2dp.bt_fd_coutq_init - coutq);

	errno = 0;
	ret = io_bt_write(th, buffer->data, ffb_blen_out(buffer));

	if (errno == EAGAIN)
		/* The io_bt_write() call was blocking due to not enough space
		 * in the BT socket. Set the coutq to some arbitrary big value. */
		coutq = 1024 * 16;

	io->coutq.i = (io->coutq.i + 1) % ARRAYSIZE(io->coutq.v);
	io->coutq.v[io->coutq.i] = coutq;

	return ret;
}

static void *a2dp_sink_sbc(struct ba_transport_thread *th) {

	/* Cancellation should be possible only in the carefully selected place
	 * in order to prevent memory leaks and resources not being released. */
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_thread_cleanup), th);

	struct ba_transport *t = th->t;
	struct a2dp_io_poll io = {
		.io.signal.filter = a2dp_io_poll_signal_filter_dec,
		.io.signal.userdata = &io,
		.io.timeout = -1,
		.th = th,
	};

	sbc_t sbc;

	if ((errno = -sbc_init_a2dp(&sbc, 0, t->a2dp.configuration,
					t->a2dp.codec->capabilities_size)) != 0) {
		error("Couldn't initialize SBC codec: %s", strerror(errno));
		goto fail_init;
	}

	ffb_t bt = { 0 };
	ffb_t pcm = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(sbc_finish), &sbc);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &pcm);

	if (ffb_init_int16_t(&pcm, sbc_get_codesize(&sbc)) == -1 ||
			ffb_init_uint8_t(&bt, t->mtu_read) == -1) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail_ffb;
	}

#if DEBUG
	uint16_t sbc_bitpool = 0;
#endif

	debug_transport_thread_loop(th, "START");
	for (ba_transport_thread_set_state_running(th);;) {

		ssize_t len;
		if ((len = a2dp_poll_and_read_bt(&io, &bt)) <= 0) {
			if (len == -1)
				error("BT poll and read error: %s", strerror(errno));
			goto fail;
		}

		if (!ba_transport_pcm_is_active(&t->a2dp.pcm))
			continue;

		const rtp_media_header_t *rtp_media_header;
		if ((rtp_media_header = a2dp_rtp_payload(bt.data, &io.rtp_seq_number)) == NULL)
			continue;

		const uint8_t *rtp_payload = (uint8_t *)(rtp_media_header + 1);
		size_t rtp_payload_len = len - (rtp_payload - (uint8_t *)bt.data);

		/* decode retrieved SBC frames */
		size_t frames = rtp_media_header->frame_count;
		while (frames--) {

			ssize_t len;
			size_t decoded;

			if ((len = sbc_decode(&sbc, rtp_payload, rtp_payload_len,
							pcm.data, ffb_blen_in(&pcm), &decoded)) < 0) {
				error("SBC decoding error: %s", strerror(-len));
				break;
			}

#if DEBUG
			if (sbc_bitpool != sbc.bitpool) {
				sbc_bitpool = sbc.bitpool;
				sbc_print_internals(&sbc);
			}
#endif

			rtp_payload += len;
			rtp_payload_len -= len;

			const size_t samples = decoded / sizeof(int16_t);
			io_pcm_scale(&t->a2dp.pcm, pcm.data, samples);
			if (io_pcm_write(&t->a2dp.pcm, pcm.data, samples) == -1)
				error("FIFO write error: %s", strerror(errno));

		}

	}

fail:
	debug_transport_thread_loop(th, "EXIT");
	ba_transport_thread_set_state_stopping(th);
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
fail_ffb:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
fail_init:
	pthread_cleanup_pop(1);
	return NULL;
}

static void *a2dp_source_sbc(struct ba_transport_thread *th) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_thread_cleanup), th);

	struct ba_transport *t = th->t;
	struct a2dp_io_poll io = {
		.io.timeout = -1,
		.th = th,
	};

	sbc_t sbc;

	if ((errno = -sbc_init_a2dp(&sbc, 0, t->a2dp.configuration,
					t->a2dp.codec->capabilities_size)) != 0) {
		error("Couldn't initialize SBC codec: %s", strerror(errno));
		goto fail_init;
	}

	ffb_t bt = { 0 };
	ffb_t pcm = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &pcm);
	pthread_cleanup_push(PTHREAD_CLEANUP(sbc_finish), &sbc);

	const a2dp_sbc_t *configuration = (a2dp_sbc_t *)t->a2dp.configuration;
	const size_t sbc_pcm_samples = sbc_get_codesize(&sbc) / sizeof(int16_t);
	const unsigned int channels = t->a2dp.pcm.channels;
	const unsigned int samplerate = t->a2dp.pcm.sampling;

	/* initialize SBC encoder bit-pool */
	sbc.bitpool = sbc_a2dp_get_bitpool(configuration, config.sbc_quality);

#if DEBUG
	sbc_print_internals(&sbc);
#endif

	/* Writing MTU should be big enough to contain RTP header, SBC payload
	 * header and at least one SBC frame. In general, there is no constraint
	 * for the MTU value, but the speed might suffer significantly. */
	const size_t mtu_write_payload = t->mtu_write - RTP_HEADER_LEN - sizeof(rtp_media_header_t);
	const size_t sbc_frame_len = sbc_get_frame_length(&sbc);

	if (mtu_write_payload < sbc_frame_len)
		warn("Writing MTU too small for one single SBC frame: %zu < %zu",
				t->mtu_write, RTP_HEADER_LEN + sizeof(rtp_media_header_t) + sbc_frame_len);

	if (ffb_init_int16_t(&pcm, sbc_pcm_samples * (mtu_write_payload / sbc_frame_len)) == -1 ||
			ffb_init_uint8_t(&bt, t->mtu_write) == -1) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail_ffb;
	}

	rtp_header_t *rtp_header;
	rtp_media_header_t *rtp_media_header;

	/* initialize RTP headers and get anchor for payload */
	uint8_t *rtp_payload = a2dp_rtp_init(bt.data, &rtp_header,
			(void **)&rtp_media_header, sizeof(*rtp_media_header));
	uint16_t seq_number = be16toh(rtp_header->seq_number);
	uint32_t timestamp = be32toh(rtp_header->timestamp);

	debug_transport_thread_loop(th, "START");
	for (ba_transport_thread_set_state_running(th);;) {

		ssize_t samples;
		if ((samples = a2dp_poll_and_read_pcm(&io, &t->a2dp.pcm, &pcm)) <= 0) {
			if (samples == -1)
				error("PCM poll and read error: %s", strerror(errno));
			ba_transport_stop_if_no_clients(t);
			continue;
		}

		/* anchor for RTP payload */
		bt.tail = rtp_payload;

		const int16_t *input = pcm.data;
		size_t input_samples = samples;
		size_t output_len = ffb_len_in(&bt);
		size_t pcm_frames = 0;
		size_t sbc_frames = 0;

		/* Generate as many SBC frames as possible, but less than a 4-bit media
		 * header frame counter can contain. The size of the output buffer is
		 * based on the socket MTU, so such transfer should be most efficient. */
		while (input_samples >= sbc_pcm_samples &&
				output_len >= sbc_frame_len &&
				sbc_frames < ((1 << 4) - 1)) {

			ssize_t len;
			ssize_t encoded;

			if ((len = sbc_encode(&sbc, input, input_samples * sizeof(int16_t),
							bt.tail, output_len, &encoded)) < 0) {
				error("SBC encoding error: %s", strerror(-len));
				break;
			}

			len = len / sizeof(int16_t);
			input += len;
			input_samples -= len;
			ffb_seek(&bt, encoded);
			output_len -= encoded;
			pcm_frames += len / channels;
			sbc_frames++;

		}

		rtp_header->seq_number = htobe16(++seq_number);
		rtp_header->timestamp = htobe32(timestamp);
		rtp_media_header->frame_count = sbc_frames;

		ssize_t ret;
		if ((ret = a2dp_write_bt(&io, &bt)) <= 0) {
			if (ret == -1)
				error("BT write error: %s", strerror(errno));
			goto fail;
		}

		/* keep data transfer at a constant bit rate, also
		 * get a timestamp for the next RTP frame */
		asrsync_sync(&io.io.asrs, pcm_frames);
		timestamp += pcm_frames * 10000 / samplerate;

		/* update busy delay (encoding overhead) */
		t->a2dp.pcm.delay = asrsync_get_busy_usec(&io.io.asrs) / 100;

		/* If the input buffer was not consumed (due to codesize limit), we
		 * have to append new data to the existing one. Since we do not use
		 * ring buffer, we will simply move unprocessed data to the front
		 * of our linear buffer. */
		ffb_shift(&pcm, samples - input_samples);

	}

fail:
	debug_transport_thread_loop(th, "EXIT");
	ba_transport_thread_set_state_stopping(th);
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
fail_ffb:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
fail_init:
	pthread_cleanup_pop(1);
	return NULL;
}

#if ENABLE_MP3LAME || ENABLE_MPG123
static void *a2dp_sink_mpeg(struct ba_transport_thread *th) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_thread_cleanup), th);

	struct ba_transport *t = th->t;
	struct a2dp_io_poll io = {
		.io.signal.filter = a2dp_io_poll_signal_filter_dec,
		.io.signal.userdata = &io,
		.io.timeout = -1,
		.th = th,
	};

#if ENABLE_MPG123

	static pthread_once_t once = PTHREAD_ONCE_INIT;
	pthread_once(&once, (void (*)(void))mpg123_init);

	int err;
	mpg123_handle *handle;
	if ((handle = mpg123_new(NULL, &err)) == NULL) {
		error("Couldn't initialize MPG123 decoder: %s", mpg123_plain_strerror(err));
		goto fail_init;
	}

	pthread_cleanup_push(PTHREAD_CLEANUP(mpg123_delete), handle);

	const unsigned int channels = t->a2dp.pcm.channels;
	const unsigned int samplerate = t->a2dp.pcm.sampling;

	mpg123_param(handle, MPG123_RESYNC_LIMIT, -1, 0);
	mpg123_param(handle, MPG123_ADD_FLAGS, MPG123_QUIET, 0);
#if MPG123_API_VERSION >= 45
	mpg123_param(handle, MPG123_ADD_FLAGS, MPG123_NO_READAHEAD, 0);
#endif

	mpg123_format_none(handle);
	if (mpg123_format(handle, samplerate, channels, MPG123_ENC_SIGNED_16) != MPG123_OK) {
		error("Couldn't set MPG123 format: %s", mpg123_strerror(handle));
		goto fail_open;
	}

	if (mpg123_open_feed(handle) != MPG123_OK) {
		error("Couldn't open MPG123 feed: %s", mpg123_strerror(handle));
		goto fail_open;
	}

	#define MPEG_PCM_DECODE_SAMPLES 4096

#else

	hip_t handle;
	if ((handle = hip_decode_init()) == NULL) {
		error("Couldn't initialize LAME decoder: %s", strerror(errno));
		goto fail_init;
	}

	const unsigned int channels = t->a2dp.pcm.channels;
	pthread_cleanup_push(PTHREAD_CLEANUP(hip_decode_exit), handle);

	/* NOTE: Size of the output buffer is "hard-coded" in hip_decode(). What is
	 *       even worse, the boundary check is so fucked-up that the hard-coded
	 *       limit can very easily overflow. In order to mitigate crash, we are
	 *       going to provide very big buffer - let's hope it will be enough. */
	#define MPEG_PCM_DECODE_SAMPLES 4096 * 100

#endif

	ffb_t bt = { 0 };
	ffb_t pcm = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &pcm);

	if (ffb_init_int16_t(&pcm, MPEG_PCM_DECODE_SAMPLES) == -1 ||
			ffb_init_uint8_t(&bt, t->mtu_read) == -1) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail_ffb;
	}

	debug_transport_thread_loop(th, "START");
	for (ba_transport_thread_set_state_running(th);;) {

		ssize_t len;
		if ((len = a2dp_poll_and_read_bt(&io, &bt)) <= 0) {
			if (len == -1)
				error("BT poll and read error: %s", strerror(errno));
			goto fail;
		}

		if (!ba_transport_pcm_is_active(&t->a2dp.pcm))
			continue;

		const rtp_mpeg_audio_header_t *rtp_mpeg_header;
		if ((rtp_mpeg_header = a2dp_rtp_payload(bt.data, &io.rtp_seq_number)) == NULL)
			continue;

		uint8_t *rtp_mpeg = (uint8_t *)(rtp_mpeg_header + 1);
		size_t rtp_mpeg_len = len - (rtp_mpeg - (uint8_t *)bt.data);

#if ENABLE_MPG123

		long rate;
		int channels;
		int encoding;

decode:
		switch (mpg123_decode(handle, rtp_mpeg, rtp_mpeg_len,
					(uint8_t *)pcm.data, ffb_blen_in(&pcm), (size_t *)&len)) {
		case MPG123_DONE:
		case MPG123_NEED_MORE:
		case MPG123_OK:
			break;
		case MPG123_NEW_FORMAT:
			mpg123_getformat(handle, &rate, &channels, &encoding);
			debug("MPG123 new format detected: r:%ld, ch:%d, enc:%#x", rate, channels, encoding);
			break;
		default:
			error("MPG123 decoding error: %s", mpg123_strerror(handle));
			continue;
		}

		const size_t samples = len / sizeof(int16_t);
		io_pcm_scale(&t->a2dp.pcm, pcm.data, samples);
		if (io_pcm_write(&t->a2dp.pcm, pcm.data, samples) == -1)
			error("FIFO write error: %s", strerror(errno));

		if (len > 0) {
			rtp_mpeg_len = 0;
			goto decode;
		}

#else

		int16_t pcm_l[MPEG_PCM_DECODE_SAMPLES];
		int16_t pcm_r[MPEG_PCM_DECODE_SAMPLES];
		ssize_t samples;

		if ((samples = hip_decode(handle, rtp_mpeg, rtp_mpeg_len, pcm_l, pcm_r)) < 0) {
			error("LAME decoding error: %zd", samples);
			continue;
		}

		if (channels == 1) {
			io_pcm_scale(&t->a2dp.pcm, pcm_l, samples);
			if (io_pcm_write(&t->a2dp.pcm, pcm_l, samples) == -1)
				error("FIFO write error: %s", strerror(errno));
		}
		else {

			ssize_t i;
			for (i = 0; i < samples; i++) {
				((int16_t *)pcm.data)[i * 2 + 0] = pcm_l[i];
				((int16_t *)pcm.data)[i * 2 + 1] = pcm_r[i];
			}

			io_pcm_scale(&t->a2dp.pcm, pcm.data, samples);
			if (io_pcm_write(&t->a2dp.pcm, pcm.data, samples) == -1)
				error("FIFO write error: %s", strerror(errno));

		}

#endif

	}

fail:
	debug_transport_thread_loop(th, "EXIT");
	ba_transport_thread_set_state_stopping(th);
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
fail_ffb:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
#if ENABLE_MPG123
fail_open:
#endif
	pthread_cleanup_pop(1);
fail_init:
	pthread_cleanup_pop(1);
	return NULL;
}
#endif

#if ENABLE_MP3LAME
static void *a2dp_source_mp3(struct ba_transport_thread *th) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_thread_cleanup), th);

	struct ba_transport *t = th->t;
	struct a2dp_io_poll io = {
		.io.timeout = -1,
		.th = th,
	};

	lame_t handle;
	if ((handle = lame_init()) == NULL) {
		error("Couldn't initialize LAME encoder: %s", strerror(errno));
		goto fail_init;
	}

	pthread_cleanup_push(PTHREAD_CLEANUP(lame_close), handle);

	const a2dp_mpeg_t *configuration = (a2dp_mpeg_t *)t->a2dp.configuration;
	const unsigned int channels = t->a2dp.pcm.channels;
	const unsigned int samplerate = t->a2dp.pcm.sampling;
	MPEG_mode mode = NOT_SET;

	lame_set_num_channels(handle, channels);
	lame_set_in_samplerate(handle, samplerate);

	switch (configuration->channel_mode) {
	case MPEG_CHANNEL_MODE_MONO:
		mode = MONO;
		break;
	case MPEG_CHANNEL_MODE_DUAL_CHANNEL:
		mode = DUAL_CHANNEL;
		break;
	case MPEG_CHANNEL_MODE_STEREO:
		mode = STEREO;
		break;
	case MPEG_CHANNEL_MODE_JOINT_STEREO:
		mode = JOINT_STEREO;
		break;
	}

	if (lame_set_mode(handle, mode) != 0) {
		error("LAME: Couldn't set mode: %d", mode);
		goto fail_setup;
	}
	if (lame_set_bWriteVbrTag(handle, 0) != 0) {
		error("LAME: Couldn't disable VBR header");
		goto fail_setup;
	}
	if (lame_set_error_protection(handle, configuration->crc) != 0) {
		error("LAME: Couldn't set CRC mode: %d", configuration->crc);
		goto fail_setup;
	}
	if (configuration->vbr) {
		if (lame_set_VBR(handle, vbr_default) != 0) {
			error("LAME: Couldn't set VBR mode: %d", vbr_default);
			goto fail_setup;
		}
		if (lame_set_VBR_q(handle, config.lame_vbr_quality) != 0) {
			error("LAME: Couldn't set VBR quality: %d", config.lame_vbr_quality);
			goto fail_setup;
		}
	}
	else {
		if (lame_set_VBR(handle, vbr_off) != 0) {
			error("LAME: Couldn't set CBR mode");
			goto fail_setup;
		}
		int mpeg_bitrate = MPEG_GET_BITRATE(*configuration);
		int bitrate = a2dp_mpeg1_mp3_get_max_bitrate(mpeg_bitrate);
		if (lame_set_brate(handle, bitrate) != 0) {
			error("LAME: Couldn't set CBR bitrate: %d", bitrate);
			goto fail_setup;
		}
		if (mpeg_bitrate & MPEG_BIT_RATE_FREE &&
				lame_set_free_format(handle, 1) != 0) {
			error("LAME: Couldn't enable free format");
			goto fail_setup;
		}
	}
	if (lame_set_quality(handle, config.lame_quality) != 0) {
		error("LAME: Couldn't set quality: %d", config.lame_quality);
		goto fail_setup;
	}

	if (lame_init_params(handle) != 0) {
		error("LAME: Couldn't setup encoder");
		goto fail_setup;
	}

	ffb_t bt = { 0 };
	ffb_t pcm = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &pcm);

	const size_t mpeg_pcm_samples = lame_get_framesize(handle);
	const size_t rtp_headers_len = RTP_HEADER_LEN + sizeof(rtp_mpeg_audio_header_t);
	/* It is hard to tell the size of the buffer required, but
	 * empirical test shows that 2KB should be sufficient. */
	const size_t mpeg_frame_len = 2048;

	if (ffb_init_int16_t(&pcm, mpeg_pcm_samples) == -1 ||
			ffb_init_uint8_t(&bt, rtp_headers_len + mpeg_frame_len) == -1) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail_ffb;
	}

	rtp_header_t *rtp_header;
	rtp_mpeg_audio_header_t *rtp_mpeg_audio_header;

	/* initialize RTP headers and get anchor for payload */
	uint8_t *rtp_payload = a2dp_rtp_init(bt.data, &rtp_header,
			(void **)&rtp_mpeg_audio_header, sizeof(*rtp_mpeg_audio_header));
	uint16_t seq_number = be16toh(rtp_header->seq_number);
	uint32_t timestamp = be32toh(rtp_header->timestamp);

	debug_transport_thread_loop(th, "START");
	for (ba_transport_thread_set_state_running(th);;) {

		ssize_t samples;
		if ((samples = a2dp_poll_and_read_pcm(&io, &t->a2dp.pcm, &pcm)) <= 0) {
			if (samples == -1)
				error("PCM poll and read error: %s", strerror(errno));
			ba_transport_stop_if_no_clients(t);
			continue;
		}

		/* anchor for RTP payload */
		bt.tail = rtp_payload;

		size_t pcm_frames = samples / channels;
		ssize_t len;

		if ((len = channels == 1 ?
					lame_encode_buffer(handle, pcm.data, NULL, pcm_frames, bt.tail, ffb_len_in(&bt)) :
					lame_encode_buffer_interleaved(handle, pcm.data, pcm_frames, bt.tail, ffb_len_in(&bt))) < 0) {
			error("LAME encoding error: %s", lame_encode_strerror(len));
			continue;
		}

		if (len > 0) {

			size_t payload_len_max = t->mtu_write - RTP_HEADER_LEN - sizeof(*rtp_mpeg_audio_header);
			size_t payload_len_total = len;
			size_t payload_len = len;
			rtp_header->timestamp = htobe32(timestamp);

			for (;;) {

				ssize_t ret;
				size_t len;

				len = payload_len > payload_len_max ? payload_len_max : payload_len;
				rtp_header->markbit = payload_len <= payload_len_max;
				rtp_header->seq_number = htobe16(++seq_number);
				rtp_mpeg_audio_header->offset = payload_len_total - payload_len;

				ffb_rewind(&bt);
				ffb_seek(&bt, RTP_HEADER_LEN + sizeof(*rtp_mpeg_audio_header) + len);

				if ((ret = a2dp_write_bt(&io, &bt)) <= 0) {
					if (ret == -1)
						error("BT write error: %s", strerror(errno));
					goto fail;
				}

				/* account written payload only */
				ret -= RTP_HEADER_LEN + sizeof(*rtp_mpeg_audio_header);

				/* break if the last part of the payload has been written */
				if ((payload_len -= ret) == 0)
					break;

				/* move rest of data to the beginning of the payload */
				debug("Payload fragmentation: extra %zd bytes", payload_len);
				memmove(rtp_payload, rtp_payload + ret, payload_len);

			}

		}

		/* keep data transfer at a constant bit rate, also
		 * get a timestamp for the next RTP frame */
		asrsync_sync(&io.io.asrs, pcm_frames);
		timestamp += pcm_frames * 10000 / samplerate;

		/* update busy delay (encoding overhead) */
		t->a2dp.pcm.delay = asrsync_get_busy_usec(&io.io.asrs) / 100;

		/* If the input buffer was not consumed (due to frame alignment), we
		 * have to append new data to the existing one. Since we do not use
		 * ring buffer, we will simply move unprocessed data to the front
		 * of our linear buffer. */
		ffb_shift(&pcm, pcm_frames * channels);

	}

fail:
	debug_transport_thread_loop(th, "EXIT");
	ba_transport_thread_set_state_stopping(th);
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
fail_ffb:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
fail_setup:
	pthread_cleanup_pop(1);
fail_init:
	pthread_cleanup_pop(1);
	return NULL;
}
#endif

#if ENABLE_AAC
static void *a2dp_sink_aac(struct ba_transport_thread *th) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_thread_cleanup), th);

	struct ba_transport *t = th->t;
	struct a2dp_io_poll io = {
		.io.signal.filter = a2dp_io_poll_signal_filter_dec,
		.io.signal.userdata = &io,
		.io.timeout = -1,
		.th = th,
	};

	HANDLE_AACDECODER handle;
	AAC_DECODER_ERROR err;

	if ((handle = aacDecoder_Open(TT_MP4_LATM_MCP1, 1)) == NULL) {
		error("Couldn't open AAC decoder");
		goto fail_open;
	}

	pthread_cleanup_push(PTHREAD_CLEANUP(aacDecoder_Close), handle);

	const unsigned int channels = t->a2dp.pcm.channels;
#ifdef AACDECODER_LIB_VL0
	if ((err = aacDecoder_SetParam(handle, AAC_PCM_MIN_OUTPUT_CHANNELS, channels)) != AAC_DEC_OK) {
		error("Couldn't set min output channels: %s", aacdec_strerror(err));
		goto fail_init;
	}
	if ((err = aacDecoder_SetParam(handle, AAC_PCM_MAX_OUTPUT_CHANNELS, channels)) != AAC_DEC_OK) {
		error("Couldn't set max output channels: %s", aacdec_strerror(err));
		goto fail_init;
	}
#else
	if ((err = aacDecoder_SetParam(handle, AAC_PCM_OUTPUT_CHANNELS, channels)) != AAC_DEC_OK) {
		error("Couldn't set output channels: %s", aacdec_strerror(err));
		goto fail_init;
	}
#endif

	ffb_t bt = { 0 };
	ffb_t latm = { 0 };
	ffb_t pcm = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &latm);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &pcm);

	if (ffb_init_int16_t(&pcm, 2048 * channels) == -1 ||
			ffb_init_uint8_t(&latm, t->mtu_read) == -1 ||
			ffb_init_uint8_t(&bt, t->mtu_read) == -1) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail_ffb;
	}

	int markbit_quirk = -3;

	debug_transport_thread_loop(th, "START");
	for (ba_transport_thread_set_state_running(th);;) {

		ssize_t len;
		if ((len = a2dp_poll_and_read_bt(&io, &bt)) <= 0) {
			if (len == -1)
				error("BT poll and read error: %s", strerror(errno));
			goto fail;
		}

		if (!ba_transport_pcm_is_active(&t->a2dp.pcm))
			continue;

		const uint8_t *rtp_latm;
		if ((rtp_latm = a2dp_rtp_payload(bt.data, &io.rtp_seq_number)) == NULL)
			continue;

		const rtp_header_t *rtp_header = (rtp_header_t *)bt.data;
		size_t rtp_latm_len = len - (rtp_latm - (uint8_t *)bt.data);

		/* If in the first N packets mark bit is not set, it might mean, that
		 * the mark bit will not be set at all. In such a case, activate mark
		 * bit quirk workaround. */
		if (markbit_quirk < 0) {
			if (rtp_header->markbit)
				markbit_quirk = 0;
			else if (++markbit_quirk == 0) {
				warn("Activating RTP mark bit quirk workaround");
				markbit_quirk = 1;
			}
		}

		if (ffb_len_in(&latm) < rtp_latm_len) {
			debug("Resizing LATM buffer: %zd -> %zd", latm.size, latm.size + t->mtu_read);
			size_t prev_len = ffb_len_out(&latm);
			ffb_init_uint8_t(&latm, latm.nmemb + t->mtu_read);
			ffb_seek(&latm, prev_len);
		}

		memcpy(latm.tail, rtp_latm, rtp_latm_len);
		ffb_seek(&latm, rtp_latm_len);

		if (markbit_quirk != 1 && !rtp_header->markbit) {
			debug("Fragmented RTP packet [%u]: LATM len: %zd", io.rtp_seq_number, rtp_latm_len);
			continue;
		}

		unsigned int data_len = ffb_len_out(&latm);
		unsigned int valid = ffb_len_out(&latm);
		CStreamInfo *aacinf;

		if ((err = aacDecoder_Fill(handle, (uint8_t **)&latm.data, &data_len, &valid)) != AAC_DEC_OK)
			error("AAC buffer fill error: %s", aacdec_strerror(err));
		else if ((err = aacDecoder_DecodeFrame(handle, pcm.tail, ffb_blen_in(&pcm), 0)) != AAC_DEC_OK)
			error("AAC decode frame error: %s", aacdec_strerror(err));
		else if ((aacinf = aacDecoder_GetStreamInfo(handle)) == NULL)
			error("Couldn't get AAC stream info");
		else {
			if ((unsigned int)aacinf->numChannels != channels)
				warn("AAC channels mismatch: %u != %u", aacinf->numChannels, channels);
			const size_t samples = (size_t)aacinf->frameSize * channels;
			io_pcm_scale(&t->a2dp.pcm, pcm.data, samples);
			if (io_pcm_write(&t->a2dp.pcm, pcm.data, samples) == -1)
				error("FIFO write error: %s", strerror(errno));
		}

		/* make room for new LATM frame */
		ffb_rewind(&latm);

	}

fail:
	debug_transport_thread_loop(th, "EXIT");
	ba_transport_thread_set_state_stopping(th);
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
fail_ffb:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
fail_init:
	pthread_cleanup_pop(1);
fail_open:
	pthread_cleanup_pop(1);
	return NULL;
}
#endif

#if ENABLE_AAC
static void *a2dp_source_aac(struct ba_transport_thread *th) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_thread_cleanup), th);

	struct ba_transport *t = th->t;
	struct a2dp_io_poll io = {
		.io.timeout = -1,
		.th = th,
	};

	HANDLE_AACENCODER handle;
	AACENC_InfoStruct aacinf;
	AACENC_ERROR err;

	const a2dp_aac_t *configuration = (a2dp_aac_t *)t->a2dp.configuration;
	const unsigned int bitrate = AAC_GET_BITRATE(*configuration);
	const unsigned int channels = t->a2dp.pcm.channels;
	const unsigned int samplerate = t->a2dp.pcm.sampling;

	/* create AAC encoder without the Meta Data module */
	if ((err = aacEncOpen(&handle, 0x07, channels)) != AACENC_OK) {
		error("Couldn't open AAC encoder: %s", aacenc_strerror(err));
		goto fail_open;
	}

	pthread_cleanup_push(PTHREAD_CLEANUP(aacEncClose), &handle);

	unsigned int aot = AOT_NONE;
	unsigned int channelmode = channels == 1 ? MODE_1 : MODE_2;

	switch (configuration->object_type) {
	case AAC_OBJECT_TYPE_MPEG2_AAC_LC:
#if AACENCODER_LIB_VERSION <= 0x03040C00 /* 3.4.12 */ || \
		AACENCODER_LIB_VERSION >= 0x04000000 /* 4.0.0 */
		aot = AOT_MP2_AAC_LC;
		break;
#endif
	case AAC_OBJECT_TYPE_MPEG4_AAC_LC:
		aot = AOT_AAC_LC;
		break;
	case AAC_OBJECT_TYPE_MPEG4_AAC_LTP:
		aot = AOT_AAC_LTP;
		break;
	case AAC_OBJECT_TYPE_MPEG4_AAC_SCA:
		aot = AOT_AAC_SCAL;
		break;
	}

	if ((err = aacEncoder_SetParam(handle, AACENC_AOT, aot)) != AACENC_OK) {
		error("Couldn't set audio object type: %s", aacenc_strerror(err));
		goto fail_init;
	}
	if ((err = aacEncoder_SetParam(handle, AACENC_BITRATE, bitrate)) != AACENC_OK) {
		error("Couldn't set bitrate: %s", aacenc_strerror(err));
		goto fail_init;
	}
	if ((err = aacEncoder_SetParam(handle, AACENC_SAMPLERATE, samplerate)) != AACENC_OK) {
		error("Couldn't set sampling rate: %s", aacenc_strerror(err));
		goto fail_init;
	}
	if ((err = aacEncoder_SetParam(handle, AACENC_CHANNELMODE, channelmode)) != AACENC_OK) {
		error("Couldn't set channel mode: %s", aacenc_strerror(err));
		goto fail_init;
	}
	if (configuration->vbr) {
		if ((err = aacEncoder_SetParam(handle, AACENC_BITRATEMODE, config.aac_vbr_mode)) != AACENC_OK) {
			error("Couldn't set VBR bitrate mode %u: %s", config.aac_vbr_mode, aacenc_strerror(err));
			goto fail_init;
		}
	}
	if ((err = aacEncoder_SetParam(handle, AACENC_AFTERBURNER, config.aac_afterburner)) != AACENC_OK) {
		error("Couldn't enable afterburner: %s", aacenc_strerror(err));
		goto fail_init;
	}
	if ((err = aacEncoder_SetParam(handle, AACENC_TRANSMUX, TT_MP4_LATM_MCP1)) != AACENC_OK) {
		error("Couldn't enable LATM transport type: %s", aacenc_strerror(err));
		goto fail_init;
	}
	if ((err = aacEncoder_SetParam(handle, AACENC_HEADER_PERIOD, 1)) != AACENC_OK) {
		error("Couldn't set LATM header period: %s", aacenc_strerror(err));
		goto fail_init;
	}
#if AACENCODER_LIB_VERSION >= 0x03041600 /* 3.4.22 */
	if ((err = aacEncoder_SetParam(handle, AACENC_AUDIOMUXVER, config.aac_latm_version)) != AACENC_OK) {
		error("Couldn't set LATM version: %s", aacenc_strerror(err));
		goto fail_init;
	}
#endif

	if ((err = aacEncEncode(handle, NULL, NULL, NULL, NULL)) != AACENC_OK) {
		error("Couldn't initialize AAC encoder: %s", aacenc_strerror(err));
		goto fail_init;
	}
	if ((err = aacEncInfo(handle, &aacinf)) != AACENC_OK) {
		error("Couldn't get encoder info: %s", aacenc_strerror(err));
		goto fail_init;
	}

	ffb_t bt = { 0 };
	ffb_t pcm = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &pcm);

	const unsigned int aac_frame_size = aacinf.inputChannels * aacinf.frameLength;
	const size_t sample_size = BA_TRANSPORT_PCM_FORMAT_BYTES(t->a2dp.pcm.format);
	if (ffb_init(&pcm, aac_frame_size, sample_size) == -1 ||
			ffb_init_uint8_t(&bt, RTP_HEADER_LEN + aacinf.maxOutBufBytes) == -1) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail_ffb;
	}

	rtp_header_t *rtp_header;

	/* initialize RTP header and get anchor for payload */
	uint8_t *rtp_payload = a2dp_rtp_init(bt.data, &rtp_header, NULL, 0);
	uint16_t seq_number = be16toh(rtp_header->seq_number);
	uint32_t timestamp = be32toh(rtp_header->timestamp);

	int in_bufferIdentifiers[] = { IN_AUDIO_DATA };
	int out_bufferIdentifiers[] = { OUT_BITSTREAM_DATA };
	int in_bufSizes[] = { pcm.nmemb * pcm.size };
	int out_bufSizes[] = { aacinf.maxOutBufBytes };
	int in_bufElSizes[] = { pcm.size };
	int out_bufElSizes[] = { bt.size };

	AACENC_BufDesc in_buf = {
		.numBufs = 1,
		.bufs = (void **)&pcm.data,
		.bufferIdentifiers = in_bufferIdentifiers,
		.bufSizes = in_bufSizes,
		.bufElSizes = in_bufElSizes,
	};
	AACENC_BufDesc out_buf = {
		.numBufs = 1,
		.bufs = (void **)&rtp_payload,
		.bufferIdentifiers = out_bufferIdentifiers,
		.bufSizes = out_bufSizes,
		.bufElSizes = out_bufElSizes,
	};
	AACENC_InArgs in_args = { 0 };
	AACENC_OutArgs out_args = { 0 };

	debug_transport_thread_loop(th, "START");
	for (ba_transport_thread_set_state_running(th);;) {

		ssize_t samples;
		if ((samples = a2dp_poll_and_read_pcm(&io, &t->a2dp.pcm, &pcm)) <= 0) {
			if (samples == -1)
				error("PCM poll and read error: %s", strerror(errno));
			ba_transport_stop_if_no_clients(t);
			continue;
		}

		while ((in_args.numInSamples = ffb_len_out(&pcm)) > 0) {

			if ((err = aacEncEncode(handle, &in_buf, &out_buf, &in_args, &out_args)) != AACENC_OK)
				error("AAC encoding error: %s", aacenc_strerror(err));

			if (out_args.numOutBytes > 0) {

				size_t payload_len_max = t->mtu_write - RTP_HEADER_LEN;
				size_t payload_len = out_args.numOutBytes;
				rtp_header->timestamp = htobe32(timestamp);

				/* If the size of the RTP packet exceeds writing MTU, the RTP payload
				 * should be fragmented. According to the RFC 3016, fragmentation of
				 * the audioMuxElement requires no extra header - the payload should
				 * be fragmented and spread across multiple RTP packets. */
				for (;;) {

					ssize_t ret;
					size_t len;

					len = payload_len > payload_len_max ? payload_len_max : payload_len;
					rtp_header->markbit = payload_len <= payload_len_max;
					rtp_header->seq_number = htobe16(++seq_number);

					ffb_rewind(&bt);
					ffb_seek(&bt, RTP_HEADER_LEN + len);

					if ((ret = a2dp_write_bt(&io, &bt)) <= 0) {
						if (ret == -1)
							error("BT write error: %s", strerror(errno));
						goto fail;
					}

					/* account written payload only */
					ret -= RTP_HEADER_LEN;

					/* break if the last part of the payload has been written */
					if ((payload_len -= ret) == 0)
						break;

					/* move rest of data to the beginning of the payload */
					debug("Payload fragmentation: extra %zd bytes", payload_len);
					memmove(rtp_payload, rtp_payload + ret, payload_len);

				}

			}

			/* keep data transfer at a constant bit rate, also
			 * get a timestamp for the next RTP frame */
			unsigned int pcm_frames = out_args.numInSamples / channels;
			asrsync_sync(&io.io.asrs, pcm_frames);
			timestamp += pcm_frames * 10000 / samplerate;

			/* update busy delay (encoding overhead) */
			t->a2dp.pcm.delay = asrsync_get_busy_usec(&io.io.asrs) / 100;

			/* If the input buffer was not consumed, we have to append new data to
			 * the existing one. Since we do not use ring buffer, we will simply
			 * move unprocessed data to the front of our linear buffer. */
			ffb_shift(&pcm, out_args.numInSamples);

		}

	}

fail:
	debug_transport_thread_loop(th, "EXIT");
	ba_transport_thread_set_state_stopping(th);
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
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

#if ENABLE_APTX && HAVE_APTX_DECODE
static void *a2dp_sink_aptx(struct ba_transport_thread *th) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_thread_cleanup), th);

	struct ba_transport *t = th->t;
	struct a2dp_io_poll io = {
		.io.timeout = -1,
		.th = th,
	};

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

	debug_transport_thread_loop(th, "START");
	for (ba_transport_thread_set_state_running(th);;) {

		ssize_t len;
		if ((len = a2dp_poll_and_read_bt(&io, &bt)) <= 0) {
			if (len == -1)
				error("BT poll and read error: %s", strerror(errno));
			goto fail;
		}

		if (!ba_transport_pcm_is_active(&t->a2dp.pcm))
			continue;

		uint8_t *input = bt.data;
		size_t input_len = len;

		ffb_rewind(&pcm);
		while (input_len >= 4) {

			size_t decoded = ffb_len_in(&pcm);
			ssize_t len;

			if ((len = aptxdec_decode(handle, input, input_len, pcm.tail, &decoded)) <= 0) {
				error("Apt-X decoding error: %s", strerror(errno));
				continue;
			}

			input += len;
			input_len -= len;
			ffb_seek(&pcm, decoded);

		}

		const size_t samples = ffb_len_out(&pcm);
		io_pcm_scale(&t->a2dp.pcm, pcm.data, samples);
		if (io_pcm_write(&t->a2dp.pcm, pcm.data, samples) == -1)
			error("FIFO write error: %s", strerror(errno));

	}

fail:
	debug_transport_thread_loop(th, "EXIT");
	ba_transport_thread_set_state_stopping(th);
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
fail_ffb:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
fail_init:
	pthread_cleanup_pop(1);
	return NULL;
}
#endif

#if ENABLE_APTX
static void *a2dp_source_aptx(struct ba_transport_thread *th) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_thread_cleanup), th);

	struct ba_transport *t = th->t;
	struct a2dp_io_poll io = {
		.io.timeout = -1,
		.th = th,
	};

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

	const unsigned int channels = t->a2dp.pcm.channels;
	const size_t aptx_pcm_samples = 4 * channels;
	const size_t aptx_code_len = 2 * sizeof(uint16_t);
	const size_t mtu_write = t->mtu_write;

	if (ffb_init_int16_t(&pcm, aptx_pcm_samples * (mtu_write / aptx_code_len)) == -1 ||
			ffb_init_uint8_t(&bt, mtu_write) == -1) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail_ffb;
	}

	debug_transport_thread_loop(th, "START");
	for (ba_transport_thread_set_state_running(th);;) {

		ssize_t samples;
		if ((samples = a2dp_poll_and_read_pcm(&io, &t->a2dp.pcm, &pcm)) <= 0) {
			if (samples == -1)
				error("PCM poll and read error: %s", strerror(errno));
			ba_transport_stop_if_no_clients(t);
			continue;
		}

		int16_t *input = pcm.data;
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

			ssize_t ret;
			if ((ret = a2dp_write_bt(&io, &bt)) <= 0) {
				if (ret == -1)
					error("BT write error: %s", strerror(errno));
				goto fail;
			}

			/* keep data transfer at a constant bit rate */
			asrsync_sync(&io.io.asrs, pcm_samples / channels);

			/* update busy delay (encoding overhead) */
			t->a2dp.pcm.delay = asrsync_get_busy_usec(&io.io.asrs) / 100;

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
	debug_transport_thread_loop(th, "EXIT");
	ba_transport_thread_set_state_stopping(th);
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
fail_ffb:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
fail_init:
	pthread_cleanup_pop(1);
	return NULL;
}
#endif

#if ENABLE_APTX_HD && HAVE_APTX_HD_DECODE
static void *a2dp_sink_aptx_hd(struct ba_transport_thread *th) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_thread_cleanup), th);

	struct ba_transport *t = th->t;
	struct a2dp_io_poll io = {
		.io.signal.filter = a2dp_io_poll_signal_filter_dec,
		.io.signal.userdata = &io,
		.io.timeout = -1,
		.th = th,
	};

	HANDLE_APTX handle;
	if ((handle = aptxhddec_init()) == NULL) {
		error("Couldn't initialize apt-X HD decoder: %s", strerror(errno));
		goto fail_init;
	}

	ffb_t bt = { 0 };
	ffb_t pcm = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &pcm);
	pthread_cleanup_push(PTHREAD_CLEANUP(aptxhddec_destroy), handle);

	/* Note, that we are allocating space for one extra output packed, which is
	 * required by the aptx_decode_sync() function of libopenaptx library. */
	if (ffb_init_int32_t(&pcm, (t->mtu_read / 6 + 1) * 8) == -1 ||
			ffb_init_uint8_t(&bt, t->mtu_read) == -1) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail_ffb;
	}

	debug_transport_thread_loop(th, "START");
	for (ba_transport_thread_set_state_running(th);;) {

		ssize_t len;
		if ((len = a2dp_poll_and_read_bt(&io, &bt)) <= 0) {
			if (len == -1)
				error("BT poll and read error: %s", strerror(errno));
			goto fail;
		}

		if (!ba_transport_pcm_is_active(&t->a2dp.pcm))
			continue;

		const uint8_t *rtp_payload;
		if ((rtp_payload = a2dp_rtp_payload(bt.data, &io.rtp_seq_number)) == NULL)
			continue;

		size_t rtp_payload_len = len - (rtp_payload - (uint8_t *)bt.data);

		ffb_rewind(&pcm);
		while (rtp_payload_len >= 6) {

			size_t decoded = ffb_len_in(&pcm);
			ssize_t len;

			if ((len = aptxhddec_decode(handle, rtp_payload, rtp_payload_len, pcm.tail, &decoded)) <= 0) {
				error("Apt-X decoding error: %s", strerror(errno));
				continue;
			}

			rtp_payload += len;
			rtp_payload_len -= len;
			ffb_seek(&pcm, decoded);

		}

		const size_t samples = ffb_len_out(&pcm);
		io_pcm_scale(&t->a2dp.pcm, pcm.data, samples);
		if (io_pcm_write(&t->a2dp.pcm, pcm.data, samples) == -1)
			error("FIFO write error: %s", strerror(errno));

	}

fail:
	debug_transport_thread_loop(th, "EXIT");
	ba_transport_thread_set_state_stopping(th);
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
fail_ffb:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
fail_init:
	pthread_cleanup_pop(1);
	return NULL;
}
#endif

#if ENABLE_APTX_HD
static void *a2dp_source_aptx_hd(struct ba_transport_thread *th) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_thread_cleanup), th);

	struct ba_transport *t = th->t;
	struct a2dp_io_poll io = {
		.io.timeout = -1,
		.th = th,
	};

	HANDLE_APTX handle;
	if ((handle = aptxhdenc_init()) == NULL) {
		error("Couldn't initialize apt-X HD encoder: %s", strerror(errno));
		goto fail_init;
	}

	ffb_t bt = { 0 };
	ffb_t pcm = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &pcm);
	pthread_cleanup_push(PTHREAD_CLEANUP(aptxhdenc_destroy), handle);

	const unsigned int channels = t->a2dp.pcm.channels;
	const unsigned int samplerate = t->a2dp.pcm.sampling;
	const size_t aptx_pcm_samples = 4 * channels;
	const size_t aptx_code_len = 2 * 3 * sizeof(uint8_t);
	const size_t mtu_write = t->mtu_write;

	if (ffb_init_int32_t(&pcm, aptx_pcm_samples * ((mtu_write - RTP_HEADER_LEN) / aptx_code_len)) == -1 ||
			ffb_init_uint8_t(&bt, mtu_write) == -1) {
		error("Couldn't create data buffers: %s", strerror(errno));
		goto fail_ffb;
	}

	rtp_header_t *rtp_header;

	/* initialize RTP header and get anchor for payload */
	uint8_t *rtp_payload = a2dp_rtp_init(bt.data, &rtp_header, NULL, 0);
	uint16_t seq_number = be16toh(rtp_header->seq_number);
	uint32_t timestamp = be32toh(rtp_header->timestamp);

	debug_transport_thread_loop(th, "START");
	for (ba_transport_thread_set_state_running(th);;) {

		ssize_t samples;
		if ((samples = a2dp_poll_and_read_pcm(&io, &t->a2dp.pcm, &pcm)) <= 0) {
			if (samples == -1)
				error("PCM poll and read error: %s", strerror(errno));
			ba_transport_stop_if_no_clients(t);
			continue;
		}

		int32_t *input = pcm.data;
		size_t input_samples = samples;

		/* encode and transfer obtained data */
		while (input_samples >= aptx_pcm_samples) {

			/* anchor for RTP payload */
			bt.tail = rtp_payload;

			size_t output_len = ffb_len_in(&bt);
			size_t pcm_samples = 0;

			/* Generate as many apt-X frames as possible to fill the output buffer
			 * without overflowing it. The size of the output buffer is based on
			 * the socket MTU, so such a transfer should be most efficient. */
			while (input_samples >= aptx_pcm_samples && output_len >= aptx_code_len) {

				size_t encoded = output_len;
				ssize_t len;

				if ((len = aptxhdenc_encode(handle, input, input_samples, bt.tail, &encoded)) <= 0) {
					error("Apt-X HD encoding error: %s", strerror(errno));
					break;
				}

				input += len;
				input_samples -= len;
				ffb_seek(&bt, encoded);
				output_len -= encoded;
				pcm_samples += len;

			}

			ssize_t ret;
			if ((ret = a2dp_write_bt(&io, &bt)) <= 0) {
				if (ret == -1)
					error("BT write error: %s", strerror(errno));
				goto fail;
			}

			/* keep data transfer at a constant bit rate */
			unsigned int pcm_frames = pcm_samples / channels;
			asrsync_sync(&io.io.asrs, pcm_frames);
			timestamp += pcm_frames * 10000 / samplerate;

			/* update busy delay (encoding overhead) */
			t->a2dp.pcm.delay = asrsync_get_busy_usec(&io.io.asrs) / 100;

			rtp_header->seq_number = htobe16(++seq_number);
			rtp_header->timestamp = htobe32(timestamp);

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
	debug_transport_thread_loop(th, "EXIT");
	ba_transport_thread_set_state_stopping(th);
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
fail_ffb:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
fail_init:
	pthread_cleanup_pop(1);
	return NULL;
}
#endif

#if ENABLE_LDAC && HAVE_LDAC_DECODE
static void *a2dp_sink_ldac(struct ba_transport_thread *th) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_thread_cleanup), th);

	struct ba_transport *t = th->t;
	struct a2dp_io_poll io = {
		.io.signal.filter = a2dp_io_poll_signal_filter_dec,
		.io.signal.userdata = &io,
		.io.timeout = -1,
		.th = th,
	};

	HANDLE_LDAC_BT handle;
	if ((handle = ldacBT_get_handle()) == NULL) {
		error("Couldn't get LDAC handle: %s", strerror(errno));
		goto fail_open;
	}

	pthread_cleanup_push(PTHREAD_CLEANUP(ldacBT_free_handle), handle);

	const a2dp_ldac_t *configuration = (a2dp_ldac_t *)t->a2dp.configuration;
	const size_t sample_size = BA_TRANSPORT_PCM_FORMAT_BYTES(t->a2dp.pcm.format);
	const unsigned int channels = t->a2dp.pcm.channels;
	const unsigned int samplerate = t->a2dp.pcm.sampling;

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

	debug_transport_thread_loop(th, "START");
	for (ba_transport_thread_set_state_running(th);;) {

		ssize_t len;
		if ((len = a2dp_poll_and_read_bt(&io, &bt)) <= 0) {
			if (len == -1)
				error("BT poll and read error: %s", strerror(errno));
			goto fail;
		}

		if (!ba_transport_pcm_is_active(&t->a2dp.pcm))
			continue;

		const rtp_media_header_t *rtp_media_header;
		if ((rtp_media_header = a2dp_rtp_payload(bt.data, &io.rtp_seq_number)) == NULL)
			continue;

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
			io_pcm_scale(&t->a2dp.pcm, pcm.data, samples);
			if (io_pcm_write(&t->a2dp.pcm, pcm.data, samples) == -1)
				error("FIFO write error: %s", strerror(errno));

		}

	}

fail:
	debug_transport_thread_loop(th, "EXIT");
	ba_transport_thread_set_state_stopping(th);
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
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

#if ENABLE_LDAC
static void *a2dp_source_ldac(struct ba_transport_thread *th) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_thread_cleanup), th);

	struct ba_transport *t = th->t;
	struct a2dp_io_poll io = {
		.io.timeout = -1,
		.th = th,
	};

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

	const a2dp_ldac_t *configuration = (a2dp_ldac_t *)t->a2dp.configuration;
	const size_t sample_size = BA_TRANSPORT_PCM_FORMAT_BYTES(t->a2dp.pcm.format);
	const unsigned int channels = t->a2dp.pcm.channels;
	const unsigned int samplerate = t->a2dp.pcm.sampling;
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
	uint8_t *rtp_payload = a2dp_rtp_init(bt.data, &rtp_header,
			(void **)&rtp_media_header, sizeof(*rtp_media_header));
	uint16_t seq_number = be16toh(rtp_header->seq_number);
	uint32_t timestamp = be32toh(rtp_header->timestamp);
	size_t ts_frames = 0;

	debug_transport_thread_loop(th, "START");
	for (ba_transport_thread_set_state_running(th);;) {

		ssize_t samples;
		if ((samples = a2dp_poll_and_read_pcm(&io, &t->a2dp.pcm, &pcm)) <= 0) {
			if (samples == -1)
				error("PCM poll and read error: %s", strerror(errno));
			ba_transport_stop_if_no_clients(t);
			continue;
		}

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

			frames = used / sample_size;
			input += frames;
			input_len -= frames;
			ffb_seek(&bt, encoded);

			ssize_t ret;
			if (encoded &&
					(ret = a2dp_write_bt(&io, &bt)) <= 0) {
				if (ret == -1)
					error("BT write error: %s", strerror(errno));
				goto fail;
			}

			if (config.ldac_abr)
				ldac_ABR_Proc(handle, handle_abr, io.coutq.v[io.coutq.i] / t->mtu_write, 1);

			/* keep data transfer at a constant bit rate */
			asrsync_sync(&io.io.asrs, frames / channels);
			ts_frames += frames;

			/* update busy delay (encoding overhead) */
			t->a2dp.pcm.delay = asrsync_get_busy_usec(&io.io.asrs) / 100;

			if (encoded) {
				timestamp += ts_frames / channels * 10000 / samplerate;
				rtp_header->seq_number = htobe16(++seq_number);
				rtp_header->timestamp = htobe32(timestamp);
				ts_frames = 0;
			}

		}

		/* If the input buffer was not consumed (due to codesize limit), we
		 * have to append new data to the existing one. Since we do not use
		 * ring buffer, we will simply move unprocessed data to the front
		 * of our linear buffer. */
		ffb_shift(&pcm, samples - input_len);

	}

fail:
	debug_transport_thread_loop(th, "EXIT");
	ba_transport_thread_set_state_stopping(th);
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
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
#endif

#if DEBUG
/**
 * Dump incoming BT data to a file. */
__attribute__ ((unused))
static void *a2dp_sink_dump(struct ba_transport_thread *th) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_thread_cleanup), th);

	struct ba_transport *t = th->t;
	struct a2dp_io_poll io = { .io.timeout = -1, .th = th };
	ffb_t bt = { 0 };
	FILE *f = NULL;
	char fname[64];
	char *ptr;

	sprintf(fname, "/tmp/ba-%s.dump", ba_transport_type_to_string(t->type));
	for (ptr = fname; *ptr != '\0'; ptr++) {
		*ptr = tolower(*ptr);
		if (*ptr == ' ' || *ptr == '(' || *ptr == ')')
			*ptr = '-';
	}

	debug("Opening BT dump file: %s", fname);
	if ((f = fopen(fname, "wb")) == NULL) {
		error("Couldn't create dump file: %s", strerror(errno));
		goto fail_open;
	}

	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(fclose), f);

	if (ffb_init_uint8_t(&bt, t->mtu_read) == -1) {
		error("Couldn't create data buffer: %s", strerror(errno));
		goto fail_ffb;
	}

	debug_transport_thread_loop(th, "START");
	for (ba_transport_thread_set_state_running(th);;) {
		ssize_t len;
		if ((len = a2dp_poll_and_read_bt(&io, &bt)) <= 0) {
			if (len == -1)
				error("BT poll and read error: %s", strerror(errno));
			goto fail;
		}
		debug("BT read: %zd", len);
		fwrite(bt.data, 1, len, f);
	}

fail:
	debug_transport_thread_loop(th, "EXIT");
	ba_transport_thread_set_state_stopping(th);
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
fail_ffb:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
fail_open:
	pthread_cleanup_pop(1);
	return NULL;
}
#endif

int a2dp_audio_thread_create(struct ba_transport *t) {

	struct ba_transport_thread *th_enc = &t->thread_enc;
	struct ba_transport_thread *th_dec = &t->thread_dec;

	if (t->type.profile & BA_TRANSPORT_PROFILE_A2DP_SOURCE)
		switch (t->type.codec) {
		case A2DP_CODEC_SBC:
			return ba_transport_thread_create(th_enc, a2dp_source_sbc, "ba-a2dp-sbc", true);
#if ENABLE_MPEG
		case A2DP_CODEC_MPEG12:
#if ENABLE_MP3LAME
			if (((a2dp_mpeg_t *)t->a2dp.configuration)->layer == MPEG_LAYER_MP3)
				return ba_transport_thread_create(th_enc, a2dp_source_mp3, "ba-a2dp-mp3", true);
#endif
			break;
#endif
#if ENABLE_AAC
		case A2DP_CODEC_MPEG24:
			return ba_transport_thread_create(th_enc, a2dp_source_aac, "ba-a2dp-aac", true);
#endif
#if ENABLE_APTX
		case A2DP_CODEC_VENDOR_APTX:
			return ba_transport_thread_create(th_enc, a2dp_source_aptx, "ba-a2dp-aptx", true);
#endif
#if ENABLE_APTX_HD
		case A2DP_CODEC_VENDOR_APTX_HD:
			return ba_transport_thread_create(th_enc, a2dp_source_aptx_hd, "ba-a2dp-aptx-hd", true);
#endif
#if ENABLE_LDAC
		case A2DP_CODEC_VENDOR_LDAC:
			return ba_transport_thread_create(th_enc, a2dp_source_ldac, "ba-a2dp-ldac", true);
#endif
		}
	else if (t->type.profile & BA_TRANSPORT_PROFILE_A2DP_SINK)
		switch (t->type.codec) {
		case A2DP_CODEC_SBC:
			return ba_transport_thread_create(th_dec, a2dp_sink_sbc, "ba-a2dp-sbc", true);
#if ENABLE_MPEG
		case A2DP_CODEC_MPEG12:
#if ENABLE_MPG123
			return ba_transport_thread_create(th_dec, a2dp_sink_mpeg, "ba-a2dp-mpeg", true);
#elif ENABLE_MP3LAME
			if (((a2dp_mpeg_t *)t->a2dp.configuration)->layer == MPEG_LAYER_MP3)
				return ba_transport_thread_create(th_dec, a2dp_sink_mpeg, "ba-a2dp-mp3", true);
#endif
			break;
#endif
#if ENABLE_AAC
		case A2DP_CODEC_MPEG24:
			return ba_transport_thread_create(th_dec, a2dp_sink_aac, "ba-a2dp-aac", true);
#endif
#if ENABLE_APTX && HAVE_APTX_DECODE
		case A2DP_CODEC_VENDOR_APTX:
			return ba_transport_thread_create(th_dec, a2dp_sink_aptx, "ba-a2dp-aptx", true);
#endif
#if ENABLE_APTX_HD && HAVE_APTX_HD_DECODE
		case A2DP_CODEC_VENDOR_APTX_HD:
			return ba_transport_thread_create(th_dec, a2dp_sink_aptx_hd, "ba-a2dp-aptx-hd", true);
#endif
#if ENABLE_LDAC && HAVE_LDAC_DECODE
		case A2DP_CODEC_VENDOR_LDAC:
			return ba_transport_thread_create(th_dec, a2dp_sink_ldac, "ba-a2dp-ldac", true);
#endif
		}

	warn("Codec not supported: %u", t->type.codec);
	return -1;
}
