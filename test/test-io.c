/*
 * test-io.c
 * Copyright (c) 2016-2022 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <check.h>
#include <glib.h>
#if ENABLE_LDAC
# include <ldacBT.h>
#endif
#if HAVE_SNDFILE
# include <sndfile.h>
#endif

#include "a2dp.h"
#include "ba-adapter.h"
#include "ba-device.h"
#include "ba-rfcomm.h"
#include "ba-transport.h"
#include "bluealsa-config.h"
#include "bluealsa-dbus.h"
#include "bluez.h"
#include "hfp.h"
#include "io.h"
#include "rtp.h"
#include "sco.h"
#include "shared/a2dp-codecs.h"
#include "shared/defs.h"
#include "shared/log.h"

#include "../src/a2dp.c"
#include "../src/a2dp-aac.c"
#include "../src/a2dp-aptx-hd.c"
#include "../src/a2dp-aptx.c"
#include "../src/a2dp-faststream.c"
#include "../src/a2dp-lc3plus.c"
#include "../src/a2dp-ldac.c"
#include "../src/a2dp-mpeg.c"
#include "../src/a2dp-sbc.c"
#include "../src/ba-transport.c"
#include "inc/btd.inc"
#include "inc/sine.inc"

#define CHECK_VERSION ( \
		(CHECK_MAJOR_VERSION << 16 & 0xff0000) | \
		(CHECK_MINOR_VERSION << 8 & 0x00ff00) | \
		(CHECK_MICRO_VERSION << 0 & 0x0000ff))

int bluealsa_dbus_pcm_register(struct ba_transport_pcm *pcm) {
	debug("%s: %p", __func__, (void *)pcm); return 0; }
void bluealsa_dbus_pcm_update(struct ba_transport_pcm *pcm, unsigned int mask) {
	debug("%s: %p %#x", __func__, (void *)pcm, mask); }
void bluealsa_dbus_pcm_unregister(struct ba_transport_pcm *pcm) {
	debug("%s: %p", __func__, (void *)pcm); }
struct ba_rfcomm *ba_rfcomm_new(struct ba_transport *sco, int fd) {
	debug("%s: %p", __func__, (void *)sco); (void)fd; return NULL; }
void ba_rfcomm_destroy(struct ba_rfcomm *r) {
	debug("%s: %p", __func__, (void *)r); }
int ba_rfcomm_send_signal(struct ba_rfcomm *r, enum ba_rfcomm_signal sig) {
	debug("%s: %p: %#x", __func__, (void *)r, sig); return 0; }
bool bluez_a2dp_set_configuration(const char *current_dbus_sep_path,
		const struct a2dp_sep *sep, GError **error) {
	debug("%s: %s", __func__, current_dbus_sep_path); (void)sep;
	(void)error; return false; }

static const a2dp_sbc_t config_sbc_44100_stereo = {
	.frequency = SBC_SAMPLING_FREQ_44100,
	.channel_mode = SBC_CHANNEL_MODE_STEREO,
	.block_length = SBC_BLOCK_LENGTH_16,
	.subbands = SBC_SUBBANDS_8,
	.allocation_method = SBC_ALLOCATION_LOUDNESS,
	.min_bitpool = SBC_MIN_BITPOOL,
	.max_bitpool = SBC_MAX_BITPOOL,
};

__attribute__ ((unused))
static a2dp_mpeg_t config_mp3_44100_stereo = {
	.layer = MPEG_LAYER_MP3,
	.channel_mode = MPEG_CHANNEL_MODE_STEREO,
	.frequency = MPEG_SAMPLING_FREQ_44100,
	MPEG_INIT_BITRATE(0xFFFF)
};

__attribute__ ((unused))
static a2dp_aac_t config_aac_44100_stereo = {
	.object_type = AAC_OBJECT_TYPE_MPEG2_AAC_LC,
	AAC_INIT_FREQUENCY(AAC_SAMPLING_FREQ_44100)
	.channels = AAC_CHANNELS_2,
	AAC_INIT_BITRATE(0xFFFF)
};

__attribute__ ((unused))
static const a2dp_aptx_t config_aptx_44100_stereo = {
	.info = A2DP_SET_VENDOR_ID_CODEC_ID(APTX_VENDOR_ID, APTX_CODEC_ID),
	.frequency = APTX_SAMPLING_FREQ_44100,
	.channel_mode = APTX_CHANNEL_MODE_STEREO,
};

__attribute__ ((unused))
static const a2dp_aptx_hd_t config_aptx_hd_44100_stereo = {
	.aptx.info = A2DP_SET_VENDOR_ID_CODEC_ID(APTX_HD_VENDOR_ID, APTX_HD_CODEC_ID),
	.aptx.frequency = APTX_SAMPLING_FREQ_44100,
	.aptx.channel_mode = APTX_CHANNEL_MODE_STEREO,
};

__attribute__ ((unused))
static const a2dp_faststream_t config_faststream_44100_16000 = {
	.info = A2DP_SET_VENDOR_ID_CODEC_ID(FASTSTREAM_VENDOR_ID, FASTSTREAM_CODEC_ID),
	.direction = FASTSTREAM_DIRECTION_MUSIC | FASTSTREAM_DIRECTION_VOICE,
	.frequency_music = FASTSTREAM_SAMPLING_FREQ_MUSIC_44100,
	.frequency_voice = FASTSTREAM_SAMPLING_FREQ_VOICE_16000,
};

__attribute__ ((unused))
static const a2dp_lc3plus_t config_lc3plus_48000_stereo = {
	.info = A2DP_SET_VENDOR_ID_CODEC_ID(LC3PLUS_VENDOR_ID, LC3PLUS_CODEC_ID),
	.frame_duration = LC3PLUS_FRAME_DURATION_050,
	.channels = LC3PLUS_CHANNELS_2,
	LC3PLUS_INIT_FREQUENCY(LC3PLUS_SAMPLING_FREQ_48000)
};

__attribute__ ((unused))
static const a2dp_ldac_t config_ldac_44100_stereo = {
	.info = A2DP_SET_VENDOR_ID_CODEC_ID(LDAC_VENDOR_ID, LDAC_CODEC_ID),
	.frequency = LDAC_SAMPLING_FREQ_44100,
	.channel_mode = LDAC_CHANNEL_MODE_STEREO,
};

static struct ba_adapter *adapter = NULL;
static struct ba_device *device1 = NULL;
static struct ba_device *device2 = NULL;
static const char *input_bt_file = NULL;
static const char *input_pcm_file = NULL;
static unsigned int aging_duration = 0;
static bool enable_vbr_mode = false;
static bool dump_data = false;
static bool packet_loss = false;

static struct bt_dump *btd = NULL;

#if HAVE_SNDFILE
static void *pcm_write_frames_sndfile_async(void *userdata) {

	struct ba_transport_pcm *pcm = userdata;
	struct pollfd pfds[] = {{ pcm->fd, POLLOUT, 0 }};

	SNDFILE *sf = NULL;
	SF_INFO sf_info = {};
	if ((sf = sf_open(input_pcm_file, SFM_READ, &sf_info)) == NULL) {
		error("Couldn't load input audio file: %s", sf_strerror(NULL));
		ck_assert_ptr_ne(sf, NULL);
	}

	for (;;) {

		union {
			int16_t s16[1024];
			int32_t s32[1024];
		} buffer;
		sf_count_t samples = 0;
		ssize_t len = 0;

		ck_assert_int_ne(poll(pfds, ARRAYSIZE(pfds), -1), -1);

		switch (pcm->format) {
		case BA_TRANSPORT_PCM_FORMAT_S16_2LE:
			samples = sf_read_short(sf, buffer.s16, ARRAYSIZE(buffer.s16));
			len = samples * sizeof(int16_t);
			break;
		case BA_TRANSPORT_PCM_FORMAT_S24_4LE:
			samples = sf_read_int(sf, buffer.s32, ARRAYSIZE(buffer.s32));
			for (sf_count_t i = 0; i < samples; i++)
				buffer.s32[i] >>= 8;
			len = samples * sizeof(int32_t);
			break;
		case BA_TRANSPORT_PCM_FORMAT_S32_4LE:
			samples = sf_read_int(sf, buffer.s32, ARRAYSIZE(buffer.s32));
			len = samples * sizeof(int32_t);
			break;
		default:
			g_assert_not_reached();
		}

		if (samples == 0)
			break;

		ck_assert_int_eq(write(pfds[0].fd, &buffer, len), len);

	};

	if (aging_duration == 0) {
		/* If we are not performign aging test, close the PCM right
		 * away. The reading loop will not wait for timeout. */
		pthread_mutex_lock(&pcm->mutex);
		ba_transport_pcm_release(pcm);
		pthread_mutex_unlock(&pcm->mutex);
	}

	sf_close(sf);
	return NULL;
}
#endif

/**
 * Write test PCM signal to the file descriptor. */
static void pcm_write_frames(struct ba_transport_pcm *pcm, size_t frames) {

	if (input_pcm_file != NULL) {
#if HAVE_SNDFILE
		pthread_t thread;
		pthread_create(&thread, NULL, pcm_write_frames_sndfile_async, pcm);
		pthread_detach(thread);
#else
		error("Loading audio files requires sndfile library!");
#endif
		return;
	}

	union {
		int16_t s16[2 * 1024];
		int32_t s32[2 * 1024];
	} pcm_sine_buffer;
	size_t pcm_sine_bytes = 0;

	switch (pcm->format) {
	case BA_TRANSPORT_PCM_FORMAT_S16_2LE:
		snd_pcm_sine_s16_2le(pcm_sine_buffer.s16, 1024, pcm->channels, 0, 1.0 / 128);
		pcm_sine_bytes = 1024 * pcm->channels * sizeof(int16_t);
		break;
	case BA_TRANSPORT_PCM_FORMAT_S24_4LE:
		snd_pcm_sine_s24_4le(pcm_sine_buffer.s32, 1024, pcm->channels, 0, 1.0 / 128);
		pcm_sine_bytes = 1024 * pcm->channels * sizeof(int32_t);
		break;
	case BA_TRANSPORT_PCM_FORMAT_S32_4LE:
		snd_pcm_sine_s32_4le(pcm_sine_buffer.s32, 1024, pcm->channels, 0, 1.0 / 128);
		pcm_sine_bytes = 1024 * pcm->channels * sizeof(int32_t);
		break;
	default:
		g_assert_not_reached();
	}

	size_t samples = pcm->channels * frames;
	size_t bytes = BA_TRANSPORT_PCM_FORMAT_BYTES(pcm->format) * samples;
	debug("PCM write samples: %zu (%zu bytes)", samples, bytes);

	if (dump_data) {
		FILE *f;
		ck_assert_ptr_ne(f = fopen("sample-sine.pcm", "w"), NULL);
		ck_assert_int_eq(fwrite(&pcm_sine_buffer, pcm_sine_bytes, 1, f), 1);
		ck_assert_int_eq(fclose(f), 0);
	}

	while (bytes > 0) {
		size_t len = pcm_sine_bytes <= bytes ? pcm_sine_bytes : bytes;
		ck_assert_int_eq(write(pcm->fd, &pcm_sine_buffer, len), len);
		bytes -= len;
	}

	if (aging_duration == 0) {
		/* If we are not performign aging test, close the PCM right
		 * away. The reading loop will not wait for timeout. */
		pthread_mutex_lock(&pcm->mutex);
		ba_transport_pcm_release(pcm);
		pthread_mutex_unlock(&pcm->mutex);
	}

}

struct bt_data {
	struct bt_data *next;
	uint8_t data[1024];
	size_t len;
};

/**
 * Linked list with generated BT data. */
static struct bt_data bt_data = { 0 };
static struct bt_data *bt_data_end;

static void bt_data_init(void) {
	bt_data_end = &bt_data;
}

static void bt_data_push(uint8_t *data, size_t len) {
	memcpy(bt_data_end->data, data, bt_data_end->len = len);
	if (bt_data_end->next == NULL) {
		bt_data_end->next = malloc(sizeof(*bt_data_end->next));
		bt_data_end->next->next = NULL;
	}
	bt_data_end = bt_data_end->next;
}

static void bt_data_write(struct ba_transport *t) {

	struct pollfd fds[] = {{ t->bt_fd, POLLOUT, 0 }};
	struct bt_data *bt_data_head = &bt_data;
	bool first_packet = true;
	char buffer[2024];
	ssize_t len;

	if (input_bt_file != NULL) {

		while ((len = bt_dump_read(btd, buffer, sizeof(buffer))) != -1) {
			if (packet_loss && random() < INT32_MAX / 3 && !first_packet) {
				debug("Simulating packet loss: Dropping BT packet!");
				continue;
			}
			ck_assert_int_ne(poll(fds, ARRAYSIZE(fds), -1), -1);
			ck_assert_int_eq(write(fds[0].fd, buffer, len), len);
			first_packet = false;
		}

	}
	else {

		for (; bt_data_head != bt_data_end; bt_data_head = bt_data_head->next) {
			len = bt_data_head->len;
			if (packet_loss && random() < INT32_MAX / 3 && !first_packet) {
				debug("Simulating packet loss: Dropping BT packet!");
				continue;
			}
			ck_assert_int_ne(poll(fds, ARRAYSIZE(fds), -1), -1);
			ck_assert_int_eq(write(fds[0].fd, bt_data_head->data, len), len);
			first_packet = false;
		}

	}

}

static pthread_cond_t test_terminate = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t test_mutex = PTHREAD_MUTEX_INITIALIZER;

static void *test_terminate_timer(void *arg) {
	sleep((uintptr_t)arg);
	pthread_cond_signal(&test_terminate);
	return NULL;
}

static void test_start_terminate_timer(unsigned int delay) {
	pthread_t thread;
	pthread_create(&thread, NULL, test_terminate_timer, (void *)(uintptr_t)delay);
	pthread_detach(thread);
}

static void *test_io_thread_dump_bt(struct ba_transport_thread *th) {

	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_thread_cleanup), th);

	struct ba_transport *t = th->t;
	struct pollfd pfds[] = {{ th->bt_fd, POLLIN, 0 }};
	struct bt_dump *btd = NULL;
	uint8_t buffer[1024];
	ssize_t len;

	if (dump_data) {
		char fname[64];
		sprintf(fname, "encoded-%s.btd", transport_type_to_fname(t->type));
		ck_assert_ptr_ne(btd = bt_dump_create(fname, t), NULL);
	}

	debug_transport_thread_loop(th, "START");
	ba_transport_thread_set_state_running(th);
	while (poll(pfds, ARRAYSIZE(pfds), 500) > 0) {

		if ((len = io_bt_read(th, buffer, sizeof(buffer))) <= 0) {
			if (len == -1)
				error("BT read error: %s", strerror(errno));
			break;
		}

		bt_data_push(buffer, len);
		hexdump("BT data", buffer, len, false);

		if (btd != NULL)
			bt_dump_write(btd, buffer, len);

	}

	if (btd != NULL)
		bt_dump_close(btd);

	/* signal termination and wait for cancellation */
	test_start_terminate_timer(0);
	sleep(3600);

	pthread_cleanup_pop(1);
	return NULL;
}

static void *test_io_thread_dump_pcm(struct ba_transport_thread *th) {

	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_thread_cleanup), th);

	struct ba_transport *t = th->t;
	struct ba_transport_pcm *t_pcm = NULL;

	switch (t->type.profile) {
	case BA_TRANSPORT_PROFILE_A2DP_SOURCE:
		t_pcm = &t->a2dp.pcm;
		break;
	case BA_TRANSPORT_PROFILE_A2DP_SINK:
		t_pcm = &t->a2dp.pcm_bc;
		break;
	case BA_TRANSPORT_PROFILE_HFP_AG:
	case BA_TRANSPORT_PROFILE_HSP_AG:
		t_pcm = &t->sco.spk_pcm;
		break;
	case BA_TRANSPORT_PROFILE_HFP_HF:
	case BA_TRANSPORT_PROFILE_HSP_HS:
		t_pcm = &t->sco.mic_pcm;
		break;
	default:
		g_assert_not_reached();
	}

	struct pollfd pfds[] = {{ t_pcm->fd, POLLIN, 0 }};
	size_t decoded_samples_total = 0;
	uint8_t buffer[2048];
	ssize_t len;

#if HAVE_SNDFILE
	SNDFILE *sf = NULL;
	SF_INFO sf_info = {
		.format = SF_FORMAT_WAV,
		.channels = t_pcm->channels,
		.samplerate = t_pcm->sampling,
	};
	switch (BA_TRANSPORT_PCM_FORMAT_WIDTH(t_pcm->format)) {
	case 8:
		sf_info.format |= SF_FORMAT_PCM_S8;
		break;
	case 16:
		sf_info.format |= SF_FORMAT_PCM_16;
		break;
	case 24:
		sf_info.format |= SF_FORMAT_PCM_24;
		break;
	case 32:
		sf_info.format |= SF_FORMAT_PCM_32;
		break;
	default:
		g_assert_not_reached();
	}
#endif

	if (dump_data) {
#if HAVE_SNDFILE
		char fname[64];
		sprintf(fname, "decoded-%s.wav", transport_type_to_fname(t->type));
		ck_assert_ptr_ne(sf = sf_open(fname, SFM_WRITE, &sf_info), NULL);
#else
		error("Dumping audio files requires sndfile library!");
#endif
	}

	debug_transport_thread_loop(th, "START");
	ba_transport_thread_set_state_running(th);
	while (poll(pfds, ARRAYSIZE(pfds), 500) > 0) {

		if ((len = read(pfds[0].fd, buffer, sizeof(buffer))) == -1) {
			debug("PCM read error: %s", strerror(errno));
			continue;
		}

		size_t sample_size = BA_TRANSPORT_PCM_FORMAT_BYTES(t_pcm->format);
		size_t samples = len / sample_size;
		debug("Decoded samples: %zd", samples);
		decoded_samples_total += samples;

#if HAVE_SNDFILE
		if (sf != NULL) {
			switch (sample_size) {
			case 2:
				sf_write_short(sf, (short *)buffer, samples);
				break;
			case 4:
				if (sf_info.format & SF_FORMAT_PCM_24)
					for (size_t i = 0; i < samples; i++)
						((int *)buffer)[i] <<= 8;
				sf_write_int(sf, (int *)buffer, samples);
				break;
			default:
				g_assert_not_reached();
			}
		}
#endif

	}

	debug("Decoded samples total [%zu frames]: %zu",
			decoded_samples_total / t_pcm->channels, decoded_samples_total);
	ck_assert_int_gt(decoded_samples_total, 0);

#if HAVE_SNDFILE
	if (sf != NULL)
		sf_close(sf);
#endif

	/* signal termination and wait for cancellation */
	test_start_terminate_timer(0);
	sleep(3600);

	pthread_cleanup_pop(1);
	return NULL;
}

/**
 * Drive PCM signal through source/sink loop. */
static void test_io(struct ba_transport *t_src, struct ba_transport *t_snk,
		void *(*enc)(struct ba_transport_thread *), void *(*dec)(struct ba_transport_thread *),
		size_t pcm_write_frames_count) {

	const char *enc_name = enc == test_io_thread_dump_pcm ? "dump-pcm" : "encode";
	const char *dec_name = dec == test_io_thread_dump_bt ? "dump-bt" : "decode";

	if (enc == test_io_thread_dump_pcm && input_pcm_file != NULL)
		return;
	if (dec == test_io_thread_dump_bt && input_bt_file != NULL)
		return;

	struct ba_transport_pcm *t_src_pcm = NULL;
	struct ba_transport_pcm *t_snk_pcm = NULL;

	switch (t_src->type.profile) {
	case BA_TRANSPORT_PROFILE_A2DP_SOURCE:
		t_src_pcm = &t_src->a2dp.pcm;
		break;
	case BA_TRANSPORT_PROFILE_A2DP_SINK:
		t_src_pcm = &t_src->a2dp.pcm_bc;
		break;
	case BA_TRANSPORT_PROFILE_HFP_AG:
	case BA_TRANSPORT_PROFILE_HSP_AG:
		t_src_pcm = &t_src->sco.spk_pcm;
		break;
	case BA_TRANSPORT_PROFILE_HFP_HF:
	case BA_TRANSPORT_PROFILE_HSP_HS:
		t_src_pcm = &t_src->sco.mic_pcm;
		break;
	default:
		g_assert_not_reached();
	}

	switch (t_snk->type.profile) {
	case BA_TRANSPORT_PROFILE_A2DP_SOURCE:
		t_snk_pcm = &t_snk->a2dp.pcm_bc;
		break;
	case BA_TRANSPORT_PROFILE_A2DP_SINK:
		t_snk_pcm = &t_snk->a2dp.pcm;
		break;
	case BA_TRANSPORT_PROFILE_HFP_AG:
	case BA_TRANSPORT_PROFILE_HSP_AG:
		t_snk_pcm = &t_snk->sco.mic_pcm;
		break;
	case BA_TRANSPORT_PROFILE_HFP_HF:
	case BA_TRANSPORT_PROFILE_HSP_HS:
		t_snk_pcm = &t_snk->sco.spk_pcm;
		break;
	default:
		g_assert_not_reached();
	}

	int bt_fds[2];
	ck_assert_int_eq(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, bt_fds), 0);
	debug("Created BT socket pair: %d, %d", bt_fds[0], bt_fds[1]);
	t_src->bt_fd = bt_fds[1];
	t_snk->bt_fd = bt_fds[0];

	int pcm_fds[2];
	ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pcm_fds), 0);
	debug("Created PCM socket pair: %d, %d", pcm_fds[0], pcm_fds[1]);
	t_src_pcm->fd = pcm_fds[1];
	t_snk_pcm->fd = pcm_fds[0];

	if (dec == test_io_thread_dump_bt)
		bt_data_init();

	if (aging_duration)
		test_start_terminate_timer(aging_duration);

	if (enc == test_io_thread_dump_pcm) {
		ck_assert_int_eq(ba_transport_thread_create(&t_snk->thread_dec, dec, dec_name, true), 0);
		ck_assert_int_eq(ba_transport_thread_create(&t_src->thread_enc, enc, enc_name, true), 0);
		bt_data_write(t_src);
	}
	else {
		ck_assert_int_eq(ba_transport_thread_create(&t_src->thread_enc, enc, enc_name, true), 0);
		ck_assert_int_eq(ba_transport_thread_create(&t_snk->thread_dec, dec, dec_name, true), 0);
		pcm_write_frames(t_snk_pcm, pcm_write_frames_count);
	}

	pthread_mutex_lock(&test_mutex);
	pthread_cond_wait(&test_terminate, &test_mutex);
	pthread_mutex_unlock(&test_mutex);

	pthread_mutex_lock(&t_src_pcm->mutex);
	ba_transport_pcm_release(t_src_pcm);
	pthread_mutex_unlock(&t_src_pcm->mutex);

	transport_thread_cancel_prepare(&t_src->thread_enc);
	transport_thread_cancel(&t_src->thread_enc);

	pthread_mutex_lock(&t_snk_pcm->mutex);
	ba_transport_pcm_release(t_snk_pcm);
	pthread_mutex_unlock(&t_snk_pcm->mutex);

	transport_thread_cancel_prepare(&t_snk->thread_dec);
	transport_thread_cancel(&t_snk->thread_dec);

}

static int test_transport_acquire(struct ba_transport *t) {
	debug("Acquire transport: %d", t->bt_fd);
	return 0;
}

static int test_transport_release_bt_a2dp(struct ba_transport *t) {
	free(t->bluez_dbus_owner); t->bluez_dbus_owner = NULL;
	return transport_release_bt_a2dp(t);
}

static struct ba_transport *test_transport_new_a2dp(
		struct ba_device *device,
		struct ba_transport_type type,
		const char *dbus_path,
		const struct a2dp_codec *codec,
		const void *configuration) {
#if DEBUG
	if (input_bt_file != NULL)
		configuration = &btd->a2dp_configuration;
#endif
	struct ba_transport *t = ba_transport_new_a2dp(device, type, ":test",
			dbus_path, codec, configuration);
	t->acquire = test_transport_acquire;
	t->release = test_transport_release_bt_a2dp;
	return t;
}

static struct ba_transport *test_transport_new_sco(
		struct ba_device *device,
		struct ba_transport_type type,
		const char *dbus_path) {
	struct ba_transport *t = ba_transport_new_sco(device, type, ":test",
			dbus_path, -1);
	t->acquire = test_transport_acquire;
	return t;
}

START_TEST(test_a2dp_sbc) {

	struct ba_transport_type ttype = {
		.profile = BA_TRANSPORT_PROFILE_A2DP_SOURCE,
		.codec = A2DP_CODEC_SBC };
	struct ba_transport *t1 = test_transport_new_a2dp(device1, ttype, "/path/sbc",
			&a2dp_sbc_source, &config_sbc_44100_stereo);
	ttype.profile = BA_TRANSPORT_PROFILE_A2DP_SINK;
	struct ba_transport *t2 = test_transport_new_a2dp(device2, ttype, "/path/sbc",
			&a2dp_sbc_sink, &config_sbc_44100_stereo);

	if (aging_duration) {
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 153 * 3;
		test_io(t1, t2, a2dp_sbc_enc_thread, a2dp_sbc_dec_thread, 4 * 1024);
	}
	else {
		debug("\n\n*** A2DP codec: SBC ***");
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 153 * 3;
		test_io(t1, t2, a2dp_sbc_enc_thread, test_io_thread_dump_bt, 2 * 1024);
		test_io(t1, t2, test_io_thread_dump_pcm, a2dp_sbc_dec_thread, 2 * 1024);
	}

	ba_transport_destroy(t1);
	ba_transport_destroy(t2);

} END_TEST

#if ENABLE_MP3LAME
START_TEST(test_a2dp_mp3) {

	config_mp3_44100_stereo.vbr = enable_vbr_mode ? 1 : 0;

	struct ba_transport_type ttype = {
		.profile = BA_TRANSPORT_PROFILE_A2DP_SOURCE,
		.codec = A2DP_CODEC_MPEG12 };
	struct ba_transport *t1 = test_transport_new_a2dp(device1, ttype, "/path/mp3",
			&a2dp_mpeg_source, &config_mp3_44100_stereo);
	ttype.profile = BA_TRANSPORT_PROFILE_A2DP_SINK;
	struct ba_transport *t2 = test_transport_new_a2dp(device2, ttype, "/path/mp3",
			&a2dp_mpeg_sink, &config_mp3_44100_stereo);

	if (aging_duration) {
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 1024;
		test_io(t1, t2, a2dp_mp3_enc_thread, a2dp_mpeg_dec_thread, 4 * 1024);
	}
	else {
		debug("\n\n*** A2DP codec: MP3 ***");
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 512;
		test_io(t1, t2, a2dp_mp3_enc_thread, test_io_thread_dump_bt, 3 * 1024);
		test_io(t1, t2, test_io_thread_dump_pcm, a2dp_mpeg_dec_thread, 3 * 1024);
	}

	ba_transport_destroy(t1);
	ba_transport_destroy(t2);

} END_TEST
#endif

#if ENABLE_AAC
START_TEST(test_a2dp_aac) {

	config.aac_afterburner = true;
	config.aac_prefer_vbr = enable_vbr_mode;
	config_aac_44100_stereo.vbr = enable_vbr_mode ? 1 : 0;

	struct ba_transport_type ttype = {
		.profile = BA_TRANSPORT_PROFILE_A2DP_SOURCE,
		.codec = A2DP_CODEC_MPEG24 };
	struct ba_transport *t1 = test_transport_new_a2dp(device1, ttype, "/path/aac",
			&a2dp_aac_source, &config_aac_44100_stereo);
	ttype.profile = BA_TRANSPORT_PROFILE_A2DP_SINK;
	struct ba_transport *t2 = test_transport_new_a2dp(device2, ttype, "/path/aac",
			&a2dp_aac_sink, &config_aac_44100_stereo);

	if (aging_duration) {
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 450;
		test_io(t1, t2, a2dp_aac_enc_thread, a2dp_aac_dec_thread, 4 * 1024);
	}
	else {
		debug("\n\n*** A2DP codec: AAC ***");
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 190;
		test_io(t1, t2, a2dp_aac_enc_thread, test_io_thread_dump_bt, 2 * 1024);
		test_io(t1, t2, test_io_thread_dump_pcm, a2dp_aac_dec_thread, 2 * 1024);
	}

	ba_transport_destroy(t1);
	ba_transport_destroy(t2);

} END_TEST
#endif

#if ENABLE_APTX
START_TEST(test_a2dp_aptx) {

	struct ba_transport_type ttype = {
		.profile = BA_TRANSPORT_PROFILE_A2DP_SOURCE,
		.codec = A2DP_CODEC_VENDOR_APTX };
	struct ba_transport *t1 = test_transport_new_a2dp(device1, ttype, "/path/aptx",
			&a2dp_aptx_source, &config_aptx_44100_stereo);
	ttype.profile = BA_TRANSPORT_PROFILE_A2DP_SINK;
	struct ba_transport *t2 = test_transport_new_a2dp(device2, ttype, "/path/aptx",
			&a2dp_aptx_sink, &config_aptx_44100_stereo);

	if (aging_duration) {
#if HAVE_APTX_DECODE
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 400;
		test_io(t1, t2, a2dp_aptx_enc_thread, a2dp_aptx_dec_thread, 4 * 1024);
#endif
	}
	else {
		debug("\n\n*** A2DP codec: apt-X ***");
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 40;
		test_io(t1, t2, a2dp_aptx_enc_thread, test_io_thread_dump_bt, 2 * 1024);
#if HAVE_APTX_DECODE
		test_io(t1, t2, test_io_thread_dump_pcm, a2dp_aptx_dec_thread, 2 * 1024);
#endif
	};

	ba_transport_destroy(t1);
	ba_transport_destroy(t2);

} END_TEST
#endif

#if ENABLE_APTX_HD
START_TEST(test_a2dp_aptx_hd) {

	struct ba_transport_type ttype = {
		.profile = BA_TRANSPORT_PROFILE_A2DP_SOURCE,
		.codec = A2DP_CODEC_VENDOR_APTX_HD };
	struct ba_transport *t1 = test_transport_new_a2dp(device1, ttype, "/path/aptxhd",
			&a2dp_aptx_hd_source, &config_aptx_hd_44100_stereo);
	ttype.profile = BA_TRANSPORT_PROFILE_A2DP_SINK;
	struct ba_transport *t2 = test_transport_new_a2dp(device2, ttype, "/path/aptxhd",
			&a2dp_aptx_hd_sink, &config_aptx_hd_44100_stereo);

	if (aging_duration) {
#if HAVE_APTX_HD_DECODE
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 600;
		test_io(t1, t2, a2dp_aptx_hd_enc_thread, a2dp_aptx_hd_dec_thread, 4 * 1024);
#endif
	}
	else {
		debug("\n\n*** A2DP codec: apt-X HD ***");
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 60;
		test_io(t1, t2, a2dp_aptx_hd_enc_thread, test_io_thread_dump_bt, 2 * 1024);
#if HAVE_APTX_HD_DECODE
		test_io(t1, t2, test_io_thread_dump_pcm, a2dp_aptx_hd_dec_thread, 2 * 1024);
#endif
	};

	ba_transport_destroy(t1);
	ba_transport_destroy(t2);

} END_TEST
#endif

#if ENABLE_FASTSTREAM
START_TEST(test_a2dp_faststream) {

	struct ba_transport_type ttype = {
		.profile = BA_TRANSPORT_PROFILE_A2DP_SOURCE,
		.codec = A2DP_CODEC_VENDOR_FASTSTREAM };
	struct ba_transport *t1 = test_transport_new_a2dp(device1, ttype, "/path/faststream",
			&a2dp_faststream_source, &config_faststream_44100_16000);
	ttype.profile = BA_TRANSPORT_PROFILE_A2DP_SINK;
	struct ba_transport *t2 = test_transport_new_a2dp(device2, ttype, "/path/faststream",
			&a2dp_faststream_sink, &config_faststream_44100_16000);

	if (aging_duration) {
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 72 * 3;
		test_io(t1, t2, a2dp_faststream_enc_thread, a2dp_faststream_dec_thread, 4 * 1024);
	}
	else {
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 72 * 3;
		debug("\n\n*** A2DP codec: FastStream MUSIC ***");
		test_io(t1, t2, a2dp_faststream_enc_thread, test_io_thread_dump_bt, 2 * 1024);
		test_io(t1, t2, test_io_thread_dump_pcm, a2dp_faststream_dec_thread, 2 * 1024);
		debug("\n\n*** A2DP codec: FastStream VOICE ***");
		test_io(t2, t1, a2dp_faststream_enc_thread, test_io_thread_dump_bt, 2 * 1024);
		test_io(t2, t1, test_io_thread_dump_pcm, a2dp_faststream_dec_thread, 2 * 1024);
	}

	ba_transport_destroy(t1);
	ba_transport_destroy(t2);

} END_TEST
#endif

#if ENABLE_LC3PLUS
START_TEST(test_a2dp_lc3plus) {

	struct ba_transport_type ttype = {
		.profile = BA_TRANSPORT_PROFILE_A2DP_SOURCE,
		.codec = A2DP_CODEC_VENDOR_LC3PLUS };
	struct ba_transport *t1 = test_transport_new_a2dp(device1, ttype, "/path/lc3plus",
			&a2dp_lc3plus_source, &config_lc3plus_48000_stereo);
	ttype.profile = BA_TRANSPORT_PROFILE_A2DP_SINK;
	struct ba_transport *t2 = test_transport_new_a2dp(device2, ttype, "/path/lc3plus",
			&a2dp_lc3plus_sink, &config_lc3plus_48000_stereo);

	if (aging_duration) {
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write =
			RTP_HEADER_LEN + sizeof(rtp_media_header_t) + 300;
		test_io(t1, t2, a2dp_lc3plus_enc_thread, a2dp_lc3plus_dec_thread, 4 * 1024);
	}
	else {
		debug("\n\n*** A2DP codec: LC3plus ***");
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write =
			RTP_HEADER_LEN + sizeof(rtp_media_header_t) + 300;
		test_io(t1, t2, a2dp_lc3plus_enc_thread, test_io_thread_dump_bt, 2 * 1024);
		test_io(t1, t2, test_io_thread_dump_pcm, a2dp_lc3plus_dec_thread, 2 * 1024);
	}

	ba_transport_destroy(t1);
	ba_transport_destroy(t2);

} END_TEST
#endif

#if ENABLE_LDAC
START_TEST(test_a2dp_ldac) {

	config.ldac_abr = true;
	config.ldac_eqmid = LDACBT_EQMID_HQ;

	struct ba_transport_type ttype = {
		.profile = BA_TRANSPORT_PROFILE_A2DP_SOURCE,
		.codec = A2DP_CODEC_VENDOR_LDAC };
	struct ba_transport *t1 = test_transport_new_a2dp(device1, ttype, "/path/ldac",
			&a2dp_ldac_source, &config_ldac_44100_stereo);
	ttype.profile = BA_TRANSPORT_PROFILE_A2DP_SINK;
	struct ba_transport *t2 = test_transport_new_a2dp(device2, ttype, "/path/ldac",
			&a2dp_ldac_sink, &config_ldac_44100_stereo);

	if (aging_duration) {
#if HAVE_LDAC_DECODE
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write =
			RTP_HEADER_LEN + sizeof(rtp_media_header_t) + 990 + 6;
		test_io(t1, t2, a2dp_ldac_enc_thread, a2dp_ldac_dec_thread, 4 * 1024);
#endif
	}
	else {
		debug("\n\n*** A2DP codec: LDAC ***");
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write =
			RTP_HEADER_LEN + sizeof(rtp_media_header_t) + 660 + 6;
		test_io(t1, t2, a2dp_ldac_enc_thread, test_io_thread_dump_bt, 2 * 1024);
#if HAVE_LDAC_DECODE
		test_io(t1, t2, test_io_thread_dump_pcm, a2dp_ldac_dec_thread, 2 * 1024);
#endif
	}

	ba_transport_destroy(t1);
	ba_transport_destroy(t2);

} END_TEST
#endif

START_TEST(test_sco_cvsd) {

	struct ba_transport_type ttype = {
		.profile = BA_TRANSPORT_PROFILE_HSP_AG };
	struct ba_transport *t1 = test_transport_new_sco(device1, ttype, "/path/sco/cvsd");
	struct ba_transport *t2 = test_transport_new_sco(device2, ttype, "/path/sco/cvsd");

	debug("\n\n*** SCO codec: CVSD ***");
	t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 48;
	test_io(t1, t2, sco_enc_thread, test_io_thread_dump_bt, 600);
	test_io(t1, t2, test_io_thread_dump_pcm, sco_dec_thread, 600);

	ba_transport_destroy(t1);
	ba_transport_destroy(t2);

} END_TEST

#if ENABLE_MSBC
START_TEST(test_sco_msbc) {

	adapter->hci.features[2] = LMP_TRSP_SCO;
	adapter->hci.features[3] = LMP_ESCO;

	struct ba_transport_type ttype = {
		.profile = BA_TRANSPORT_PROFILE_HFP_AG,
		.codec = HFP_CODEC_MSBC };
	struct ba_transport *t1 = test_transport_new_sco(device1, ttype, "/path/sco/msbc");
	struct ba_transport *t2 = test_transport_new_sco(device2, ttype, "/path/sco/msbc");

	debug("\n\n*** SCO codec: mSBC ***");
	t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 24;
	test_io(t1, t2, sco_enc_thread, test_io_thread_dump_bt, 600);
	test_io(t1, t2, test_io_thread_dump_pcm, sco_dec_thread, 600);

	ba_transport_destroy(t1);
	ba_transport_destroy(t2);

} END_TEST
#endif

int main(int argc, char *argv[]) {

	const struct {
		const char *name;
#if CHECK_VERSION >= 0x000D00 /* 0.13.0 */
		const TTest *tf;
#else
		TFun tf;
#endif
	} codecs[] = {
		{ a2dp_codecs_codec_id_to_string(A2DP_CODEC_SBC), test_a2dp_sbc },
#if ENABLE_MP3LAME
		{ a2dp_codecs_codec_id_to_string(A2DP_CODEC_MPEG12), test_a2dp_mp3 },
#endif
#if ENABLE_AAC
		{ a2dp_codecs_codec_id_to_string(A2DP_CODEC_MPEG24), test_a2dp_aac },
#endif
#if ENABLE_APTX
		{ a2dp_codecs_codec_id_to_string(A2DP_CODEC_VENDOR_APTX), test_a2dp_aptx },
#endif
#if ENABLE_APTX_HD
		{ a2dp_codecs_codec_id_to_string(A2DP_CODEC_VENDOR_APTX_HD), test_a2dp_aptx_hd },
#endif
#if ENABLE_FASTSTREAM
		{ a2dp_codecs_codec_id_to_string(A2DP_CODEC_VENDOR_FASTSTREAM), test_a2dp_faststream },
#endif
#if ENABLE_LC3PLUS
		{ a2dp_codecs_codec_id_to_string(A2DP_CODEC_VENDOR_LC3PLUS), test_a2dp_lc3plus },
#endif
#if ENABLE_LDAC
		{ a2dp_codecs_codec_id_to_string(A2DP_CODEC_VENDOR_LDAC), test_a2dp_ldac },
#endif
		{ hfp_codec_id_to_string(HFP_CODEC_CVSD), test_sco_cvsd },
#if ENABLE_MSBC
		{ hfp_codec_id_to_string(HFP_CODEC_MSBC), test_sco_msbc },
#endif
	};

	int opt;
	const char *opts = "ha:dl";
	struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "aging", required_argument, NULL, 'a' },
		{ "dump", no_argument, NULL, 'd' },
		{ "packet-loss", no_argument, NULL, 'l' },
		{ "input-bt", required_argument, NULL, 1 },
		{ "input-pcm", required_argument, NULL, 2 },
		{ "vbr", no_argument, NULL, 3 },
		{ 0, 0, 0, 0 },
	};

	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1)
		switch (opt) {
		case 'h' /* --help */ :
			printf("Usage:\n"
					"  %s [OPTION]... [CODEC]...\n"
					"\nOptions:\n"
					"  -h, --help\t\tprint this help and exit\n"
					"  -a, --aging=SEC\tperform aging test for SEC seconds\n"
					"  -d, --dump\t\tdump PCM and Bluetooth data\n"
					"  -l, --packet-loss\tsimulate packet loss events\n"
					"  --input-bt=FILE\tload Bluetooth data from FILE\n"
					"  --input-pcm=FILE\tload audio from FILE (via libsndfile)\n"
					"  --vbr\t\t\tuse VBR if supported by the codec\n",
					argv[0]);
			return EXIT_SUCCESS;
		case 'a' /* --aging=SEC */ :
			aging_duration = atoi(optarg);
			break;
		case 'd' /* --dump */ :
			dump_data = true;
			break;
		case 'l' /* --packet-loss */ :
			packet_loss = true;
			break;
		case 1 /* --input-bt=FILE */ :
			input_bt_file = optarg;
			break;
		case 2 /* --input-pcm=FILE */ :
			input_pcm_file = optarg;
			break;
		case 3 /* --vbr */ :
			enable_vbr_mode = true;
			break;
		default:
			fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
			return 1;
		}

	unsigned int enabled_codecs = 0xFFFF;

	if (optind != argc)
		enabled_codecs = 0;

	for (; optind < argc; optind++)
		for (size_t i = 0; i < ARRAYSIZE(codecs); i++)
			if (strcasecmp(argv[optind], codecs[i].name) == 0)
				enabled_codecs |= 1 << i;

	if (input_bt_file != NULL) {

		if ((btd = bt_dump_open(input_bt_file)) == NULL) {
			error("Couldn't open input BT dump file: %s", strerror(errno));
			return EXIT_FAILURE;
		}

		const char *codec = "";
		switch (btd->mode) {
		case BT_DUMP_MODE_A2DP_SOURCE:
		case BT_DUMP_MODE_A2DP_SINK:
			codec = a2dp_codecs_codec_id_to_string(btd->transport_codec_id);
			debug("BT dump A2DP codec: %s (%#x)", codec, btd->transport_codec_id);
			hexdump("BT dump A2DP configuration",
					&btd->a2dp_configuration, btd->a2dp_configuration_size, true);
			break;
		case BT_DUMP_MODE_SCO:
			codec = hfp_codec_id_to_string(btd->transport_codec_id);
			debug("BT dump HFP codec: %s (%#x)", codec, btd->transport_codec_id);
			break;
		}

		enabled_codecs = 0;
		for (size_t i = 0; i < ARRAYSIZE(codecs); i++)
			if (strcmp(codec, codecs[i].name) == 0)
				enabled_codecs = 1 << i;

	}

	bdaddr_t addr1 = {{ 1, 2, 3, 4, 5, 6 }};
	bdaddr_t addr2 = {{ 1, 2, 3, 7, 8, 9 }};
	adapter = ba_adapter_new(0);
	device1 = ba_device_new(adapter, &addr1);
	device2 = ba_device_new(adapter, &addr2);

	Suite *s = suite_create(__FILE__);
	TCase *tc = tcase_create(__FILE__);
	SRunner *sr = srunner_create(s);

	suite_add_tcase(s, tc);

	tcase_set_timeout(tc, aging_duration + 5);
	if (input_bt_file != NULL || input_pcm_file != NULL)
		tcase_set_timeout(tc, aging_duration + 3600);

	for (size_t i = 0; i < ARRAYSIZE(codecs); i++)
		if (enabled_codecs & (1 << i))
			tcase_add_test(tc, codecs[i].tf);

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);
	srunner_free(sr);

	return nf == 0 ? 0 : 1;
}
