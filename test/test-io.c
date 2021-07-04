/*
 * test-io.c
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
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
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <unistd.h>

#include <bluetooth/bluetooth.h>
#include <check.h>
#include <glib.h>
#if ENABLE_LDAC
# include <ldacBT.h>
#endif

#include "a2dp-codecs.h"
#include "a2dp-rtp.h"
#include "a2dp.h"
#include "ba-adapter.h"
#include "ba-device.h"
#include "ba-rfcomm.h"
#include "ba-transport.h"
#include "bluealsa-dbus.h"
#include "bluealsa.h"
#include "bluez.h"
#include "hfp.h"
#include "sco.h"
#include "utils.h"
#include "shared/defs.h"
#include "shared/log.h"

#include "../src/a2dp.c"
#include "../src/a2dp-audio.c"
#include "../src/ba-transport.c"
#include "inc/sine.inc"

unsigned int bluealsa_dbus_pcm_register(struct ba_transport_pcm *pcm, GError **error) {
	debug("%s: %p", __func__, (void *)pcm); (void)error; return 0; }
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

static const a2dp_mpeg_t config_mp3_44100_stereo = {
	.layer = MPEG_LAYER_MP3,
	.channel_mode = MPEG_CHANNEL_MODE_STEREO,
	.frequency = MPEG_SAMPLING_FREQ_44100,
	.vbr = 1,
	MPEG_INIT_BITRATE(0xFFFF)
};

static const a2dp_aac_t config_aac_44100_stereo = {
	.object_type = AAC_OBJECT_TYPE_MPEG2_AAC_LC,
	AAC_INIT_FREQUENCY(AAC_SAMPLING_FREQ_44100)
	.channels = AAC_CHANNELS_2,
	.vbr = 1,
	AAC_INIT_BITRATE(0xFFFF)
};

static const a2dp_aptx_t config_aptx_44100_stereo = {
	.info = A2DP_SET_VENDOR_ID_CODEC_ID(APTX_VENDOR_ID, APTX_CODEC_ID),
	.frequency = APTX_SAMPLING_FREQ_44100,
	.channel_mode = APTX_CHANNEL_MODE_STEREO,
};

static const a2dp_aptx_hd_t config_aptx_hd_44100_stereo = {
	.aptx.info = A2DP_SET_VENDOR_ID_CODEC_ID(APTX_HD_VENDOR_ID, APTX_HD_CODEC_ID),
	.aptx.frequency = APTX_SAMPLING_FREQ_44100,
	.aptx.channel_mode = APTX_CHANNEL_MODE_STEREO,
};

static const a2dp_ldac_t config_ldac_44100_stereo = {
	.info = A2DP_SET_VENDOR_ID_CODEC_ID(LDAC_VENDOR_ID, LDAC_CODEC_ID),
	.frequency = LDAC_SAMPLING_FREQ_44100,
	.channel_mode = LDAC_CHANNEL_MODE_STEREO,
};

static struct ba_adapter *adapter = NULL;
static struct ba_device *device1 = NULL;
static struct ba_device *device2 = NULL;
static const char *input_pcm_file = NULL;
static unsigned int aging_duration = 0;
static bool dump_data = false;

static void *write_test_pcm_async(void *userdata) {

	int fd_out = (uintptr_t)userdata;
	int fd_in;

	ck_assert_int_ne(fd_in = open(input_pcm_file, O_RDONLY), -1);

	struct pollfd pfds[] = {{ fd_out, POLLOUT, 0 }};
	ssize_t len;

	do {
		ck_assert_int_ne(poll(pfds, ARRAYSIZE(pfds), -1), -1);
		ck_assert_int_ne(len = sendfile(fd_out, fd_in, NULL, 0x7FFFF000), -1);
	} while (len > 0);

	close(fd_in);
	return NULL;
}

/**
 * Write test PCM signal to the file descriptor. */
static void write_test_pcm(int fd, int channels, size_t frames) {

	static int16_t pcm_mono_buffer[1024];
	static const size_t pcm_mono_samples = ARRAYSIZE(pcm_mono_buffer);
	static int16_t pcm_stereo_buffer[2 * 1024];
	static const size_t pcm_stereo_samples = ARRAYSIZE(pcm_stereo_buffer);
	static bool initialized = false;
	size_t samples = channels * frames;
	pthread_t thread;
	FILE *f;

	if (input_pcm_file != NULL) {
		pthread_create(&thread, NULL, write_test_pcm_async, (void *)(uintptr_t)fd);
		pthread_detach(thread);
		return;
	}

	if (!initialized) {

		/* initialize build-in sample PCM test signals */
		snd_pcm_sine_s16le(pcm_mono_buffer, pcm_mono_samples, 1, 0, 1.0 / 128);
		snd_pcm_sine_s16le(pcm_stereo_buffer, pcm_stereo_samples, 2, 0, 1.0 / 128);
		initialized = true;

		if (dump_data) {
			ck_assert_ptr_ne(f = fopen("sample-mono.pcm", "w"), NULL);
			fwrite(pcm_mono_buffer, 2, pcm_mono_samples, f);
			fclose(f);
			ck_assert_ptr_ne(f = fopen("sample-stereo.pcm", "w"), NULL);
			fwrite(pcm_stereo_buffer, 2, pcm_stereo_samples, f);
			fclose(f);
		}

	}

	int16_t *pcm_buffer = NULL;
	size_t pcm_samples = 0;

	switch (channels) {
	case 1:
		pcm_buffer = pcm_mono_buffer;
		pcm_samples = pcm_mono_samples;
		break;
	case 2:
		pcm_buffer = pcm_stereo_buffer;
		pcm_samples = pcm_stereo_samples;
		break;
	}

	ck_assert_ptr_ne(pcm_buffer, NULL);
	ck_assert_int_ne(pcm_samples, 0);

	while (samples > 0) {
		size_t _samples = pcm_samples <= samples ? pcm_samples : samples;
		size_t bytes = _samples * sizeof(*pcm_buffer);
		ck_assert_int_eq(write(fd, pcm_buffer, bytes), bytes);
		samples -= _samples;
	}

}

struct bt_data {
	struct bt_data *next;
	uint8_t data[1024];
	size_t len;
};

/**
 * Linked list with generated BT data. */
static struct bt_data bt_data = { NULL };
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

static void bt_data_write(int fd) {
	struct bt_data *bt_data_head = &bt_data;
	struct pollfd pfds[] = {{ fd, POLLOUT, 0 }};
	for (; bt_data_head != bt_data_end; bt_data_head = bt_data_head->next) {
		ck_assert_int_ne(poll(pfds, ARRAYSIZE(pfds), -1), -1);
		ck_assert_int_eq(write(fd, bt_data_head->data, bt_data_head->len), bt_data_head->len);
	}
}

static pthread_cond_t test_a2dp_terminate = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t test_a2dp_mutex = PTHREAD_MUTEX_INITIALIZER;

static void *test_a2dp_terminate_timer(void *arg) {
	sleep((uintptr_t)arg);
	pthread_cond_signal(&test_a2dp_terminate);
	return NULL;
}

static void test_a2dp_start_terminate_timer(unsigned int delay) {
	pthread_t thread;
	pthread_create(&thread, NULL, test_a2dp_terminate_timer, (void *)(uintptr_t)delay);
	pthread_detach(thread);
}

static void *test_io_thread_a2dp_dump_bt(struct ba_transport_thread *th) {

	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_thread_cleanup), th);

	struct pollfd pfds[] = {{ th->bt_fd, POLLIN, 0 }};
	uint8_t buffer[1024];
	ssize_t len;

	debug_transport_thread_loop(th, "START");
	ba_transport_thread_set_state_running(th);
	while (poll(pfds, ARRAYSIZE(pfds), 500) > 0) {

		if ((len = read(pfds[0].fd, buffer, sizeof(buffer))) == -1) {
			debug("BT read error: %s", strerror(errno));
			continue;
		}

		bt_data_push(buffer, len);

		char label[35];
		sprintf(label, "BT data [len: %3zd]", len);
		hexdump(label, buffer, len);

	}

	/* signal termination and wait for cancellation */
	test_a2dp_start_terminate_timer(0);
	sleep(3600);

	pthread_cleanup_pop(1);
	return NULL;
}

static void *test_io_thread_a2dp_dump_pcm(struct ba_transport_thread *th) {

	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_thread_cleanup), th);

	struct ba_transport *t = th->t;
	struct pollfd pfds[] = {{ t->a2dp.pcm.fd, POLLIN, 0 }};
	size_t decoded_samples_total = 0;
	int16_t buffer[2048];
	ssize_t len;
	char fname[64];
	FILE *f = NULL;

	if (dump_data) {
		sprintf(fname, "decoded-%s.pcm", ba_transport_codecs_a2dp_to_string(t->type.codec));
		ck_assert_ptr_ne(f = fopen(fname, "w"), NULL);
	}

	debug_transport_thread_loop(th, "START");
	ba_transport_thread_set_state_running(th);
	while (poll(pfds, ARRAYSIZE(pfds), 500) > 0) {

		if ((len = read(pfds[0].fd, buffer, sizeof(buffer))) == -1) {
			debug("PCM read error: %s", strerror(errno));
			continue;
		}

		size_t sample_size = BA_TRANSPORT_PCM_FORMAT_BYTES(t->a2dp.pcm.format);
		debug("Decoded samples: %zd", len / sample_size);
		decoded_samples_total += len / sample_size;

		if (f != NULL)
			fwrite(buffer, 1, len, f);
	}

	debug("Decoded samples total: %zd", decoded_samples_total);
	ck_assert_int_gt(decoded_samples_total, 0);

	if (f != NULL)
		fclose(f);

	/* signal termination and wait for cancellation */
	test_a2dp_start_terminate_timer(0);
	sleep(3600);

	pthread_cleanup_pop(1);
	return NULL;
}

/**
 * Adjust this variable in order to inject more PCM samples during A2DP test
 * initialization. Remember to restore it to its original value afterwards. */
static int test_a2dp_pcm_samples_boost = 1;

/**
 * Drive PCM signal through A2DP source/sink loop. */
static void test_a2dp(struct ba_transport *t1, struct ba_transport *t2,
		void *(*enc)(struct ba_transport_thread *), void *(*dec)(struct ba_transport_thread *)) {

	const char *enc_name = enc == test_io_thread_a2dp_dump_pcm ? "dump-pcm" : "encode";
	const char *dec_name = dec == test_io_thread_a2dp_dump_bt ? "dump-bt" : "decode";

	int bt_fds[2];
	int pcm_fds[2];

	ck_assert_int_eq(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, bt_fds), 0);
	debug("Created BT socket pair: %d, %d", bt_fds[0], bt_fds[1]);
	ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pcm_fds), 0);
	debug("Created PCM socket pair: %d, %d", pcm_fds[0], pcm_fds[1]);

	if (dec == test_io_thread_a2dp_dump_bt)
		bt_data_init();

	t1->bt_fd = bt_fds[1];
	t2->bt_fd = bt_fds[0];
	t1->a2dp.pcm.fd = pcm_fds[1];
	t2->a2dp.pcm.fd = pcm_fds[0];

	if (aging_duration)
		test_a2dp_start_terminate_timer(aging_duration);

	if (enc == test_io_thread_a2dp_dump_pcm) {
		ck_assert_int_eq(ba_transport_thread_create(&t2->thread_dec, dec, dec_name, true), 0);
		ck_assert_int_eq(ba_transport_thread_create(&t1->thread_enc, enc, enc_name, true), 0);
		bt_data_write(bt_fds[1]);
	}
	else {
		ck_assert_int_eq(ba_transport_thread_create(&t1->thread_enc, enc, enc_name, true), 0);
		write_test_pcm(pcm_fds[0], t1->a2dp.pcm.channels, 4 * 1024 * test_a2dp_pcm_samples_boost);
		ck_assert_int_eq(ba_transport_thread_create(&t2->thread_dec, dec, dec_name, true), 0);
	}

	pthread_mutex_lock(&test_a2dp_mutex);
	pthread_cond_wait(&test_a2dp_terminate, &test_a2dp_mutex);
	pthread_mutex_unlock(&test_a2dp_mutex);

	pthread_mutex_lock(&t1->a2dp.pcm.mutex);
	ba_transport_pcm_release(&t1->a2dp.pcm);
	pthread_mutex_unlock(&t1->a2dp.pcm.mutex);

	transport_thread_cancel(&t1->thread_enc);

	pthread_mutex_lock(&t2->a2dp.pcm.mutex);
	ba_transport_pcm_release(&t2->a2dp.pcm);
	pthread_mutex_unlock(&t2->a2dp.pcm.mutex);

	transport_thread_cancel(&t2->thread_dec);

}

static void test_sco(struct ba_transport *t,
		void *(*enc)(struct ba_transport_thread *), void *(*dec)(struct ba_transport_thread *)) {

	int sco_fds[2];
	int pcm_mic_fds[2];
	int pcm_spk_fds[2];

	ck_assert_int_eq(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sco_fds), 0);
	debug("Created BT socket pair: %d, %d", sco_fds[0], sco_fds[1]);
	ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM, 0, pcm_mic_fds), 0);
	debug("Created PCM mic socket pair: %d, %d", pcm_mic_fds[0], pcm_mic_fds[1]);
	ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM, 0, pcm_spk_fds), 0);
	debug("Created PCM spk socket pair: %d, %d", pcm_spk_fds[0], pcm_spk_fds[1]);
	write_test_pcm(pcm_spk_fds[0], t->sco.spk_pcm.channels, 1024);

	t->bt_fd = sco_fds[1];
	t->sco.mic_pcm.fd = pcm_mic_fds[1];
	t->sco.spk_pcm.fd = pcm_spk_fds[1];

	ck_assert_int_eq(ba_transport_thread_create(&t->thread_enc, enc, "sco-enc", true), 0);
	ck_assert_int_eq(ba_transport_thread_create(&t->thread_dec, dec, "sco-dec", false), 0);

	struct pollfd pfds[] = {
		{ sco_fds[0], POLLIN, 0 },
		{ pcm_mic_fds[0], POLLIN, 0 }};
	size_t decoded_samples_total = 0;
	uint8_t buffer[1024];
	ssize_t len;

	while (poll(pfds, ARRAYSIZE(pfds), 500) > 0) {

		if (pfds[0].revents & POLLIN) {

			ck_assert_int_gt(len = read(sco_fds[0], buffer, t->mtu_write), 0);
			ck_assert_int_gt(write(sco_fds[0], buffer, len), 0);

			char label[35];
			sprintf(label, "BT data [len: %3zd]", len);
			hexdump(label, buffer, len);

		}

		if (pfds[1].revents & POLLIN) {
			ck_assert_int_gt(len = read(pcm_mic_fds[0], buffer, sizeof(buffer)), 0);
			size_t sample_size = BA_TRANSPORT_PCM_FORMAT_BYTES(t->sco.mic_pcm.format);
			debug("Decoded samples: %zd", len / sample_size);
			decoded_samples_total += len / sample_size;
		}

	}

	debug("Decoded samples total: %zd", decoded_samples_total);
	ck_assert_int_gt(decoded_samples_total, 0);

	transport_thread_cancel(&t->thread_enc);

	close(pcm_spk_fds[0]);
	close(pcm_mic_fds[0]);
	close(sco_fds[0]);

}

static int test_transport_acquire(struct ba_transport *t) {
	debug("Acquire transport: %d", t->bt_fd);
	return 0;
}

static int test_transport_release_bt_a2dp(struct ba_transport *t) {
	free(t->bluez_dbus_owner); t->bluez_dbus_owner = NULL;
	return transport_release_bt_a2dp(t);
}

START_TEST(test_a2dp_sbc) {

	struct ba_transport_type ttype = {
		.profile = BA_TRANSPORT_PROFILE_A2DP_SOURCE,
		.codec = A2DP_CODEC_SBC };
	struct ba_transport *t1 = ba_transport_new_a2dp(device1, ttype, ":test", "/path/sbc",
			&a2dp_codec_source_sbc, &config_sbc_44100_stereo);
	ttype.profile = BA_TRANSPORT_PROFILE_A2DP_SINK;
	struct ba_transport *t2 = ba_transport_new_a2dp(device2, ttype, ":test", "/path/sbc",
			&a2dp_codec_sink_sbc, &config_sbc_44100_stereo);

	t1->acquire = t2->acquire = test_transport_acquire;
	t1->release = t2->release = test_transport_release_bt_a2dp;

	if (aging_duration) {
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 153 * 3;
		test_a2dp(t1, t2, a2dp_source_sbc, a2dp_sink_sbc);
	}
	else {
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 153 * 3;
		test_a2dp(t1, t2, a2dp_source_sbc, test_io_thread_a2dp_dump_bt);
		test_a2dp(t1, t2, test_io_thread_a2dp_dump_pcm, a2dp_sink_sbc);
	}

	ba_transport_destroy(t1);
	ba_transport_destroy(t2);

} END_TEST

#if ENABLE_MP3LAME
START_TEST(test_a2dp_mp3) {

	struct ba_transport_type ttype = {
		.profile = BA_TRANSPORT_PROFILE_A2DP_SOURCE,
		.codec = A2DP_CODEC_MPEG12 };
	struct ba_transport *t1 = ba_transport_new_a2dp(device1, ttype, ":test", "/path/mp3",
			&a2dp_codec_source_mpeg, &config_mp3_44100_stereo);
	ttype.profile = BA_TRANSPORT_PROFILE_A2DP_SINK;
	struct ba_transport *t2 = ba_transport_new_a2dp(device2, ttype, ":test", "/path/mp3",
			&a2dp_codec_sink_mpeg, &config_mp3_44100_stereo);

	t1->acquire = t2->acquire = test_transport_acquire;
	t1->release = t2->release = test_transport_release_bt_a2dp;

	int prev = test_a2dp_pcm_samples_boost;
	/* We have to feed more samples while using MP3, because mpg123 library
	 * does not output samples until at least two frames are decoded, where
	 * one frame is 4094 samples. */
	test_a2dp_pcm_samples_boost = 3;

	if (aging_duration) {
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 1024;
		test_a2dp(t1, t2, a2dp_source_mp3, a2dp_sink_mpeg);
	}
	else {
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 250;
		test_a2dp(t1, t2, a2dp_source_mp3, test_io_thread_a2dp_dump_bt);
		test_a2dp(t1, t2, test_io_thread_a2dp_dump_pcm, a2dp_sink_mpeg);
	}

	test_a2dp_pcm_samples_boost = prev;

	ba_transport_destroy(t1);
	ba_transport_destroy(t2);

} END_TEST
#endif

#if ENABLE_AAC
START_TEST(test_a2dp_aac) {

	struct ba_transport_type ttype = {
		.profile = BA_TRANSPORT_PROFILE_A2DP_SOURCE,
		.codec = A2DP_CODEC_MPEG24 };
	struct ba_transport *t1 = ba_transport_new_a2dp(device1, ttype, ":test", "/path/aac",
			&a2dp_codec_source_aac, &config_aac_44100_stereo);
	ttype.profile = BA_TRANSPORT_PROFILE_A2DP_SINK;
	struct ba_transport *t2 = ba_transport_new_a2dp(device2, ttype, ":test", "/path/aac",
			&a2dp_codec_sink_aac, &config_aac_44100_stereo);

	t1->acquire = t2->acquire = test_transport_acquire;
	t1->release = t2->release = test_transport_release_bt_a2dp;

	if (aging_duration) {
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 450;
		test_a2dp(t1, t2, a2dp_source_aac, a2dp_sink_aac);
	}
	else {
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 64;
		test_a2dp(t1, t2, a2dp_source_aac, test_io_thread_a2dp_dump_bt);
		test_a2dp(t1, t2, test_io_thread_a2dp_dump_pcm, a2dp_sink_aac);
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
	struct ba_transport *t1 = ba_transport_new_a2dp(device1, ttype, ":test", "/path/aptx",
			&a2dp_codec_source_aptx, &config_aptx_44100_stereo);
	ttype.profile = BA_TRANSPORT_PROFILE_A2DP_SINK;
	struct ba_transport *t2 = ba_transport_new_a2dp(device2, ttype, ":test", "/path/aptx",
			&a2dp_codec_sink_aptx, &config_aptx_44100_stereo);

	t1->acquire = t2->acquire = test_transport_acquire;
	t1->release = t2->release = test_transport_release_bt_a2dp;

	if (aging_duration) {
#if HAVE_APTX_DECODE
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 400;
		test_a2dp(t1, t2, a2dp_source_aptx, a2dp_sink_aptx);
#endif
	}
	else {
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 40;
		test_a2dp(t1, t2, a2dp_source_aptx, test_io_thread_a2dp_dump_bt);
#if HAVE_APTX_DECODE
		test_a2dp(t1, t2, test_io_thread_a2dp_dump_pcm, a2dp_sink_aptx);
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
	struct ba_transport *t1 = ba_transport_new_a2dp(device1, ttype, ":test", "/path/aptxhd",
			&a2dp_codec_source_aptx_hd, &config_aptx_hd_44100_stereo);
	ttype.profile = BA_TRANSPORT_PROFILE_A2DP_SINK;
	struct ba_transport *t2 = ba_transport_new_a2dp(device2, ttype, ":test", "/path/aptxhd",
			&a2dp_codec_sink_aptx_hd, &config_aptx_hd_44100_stereo);

	t1->acquire = t2->acquire = test_transport_acquire;
	t1->release = t2->release = test_transport_release_bt_a2dp;

	if (aging_duration) {
#if HAVE_APTX_HD_DECODE
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 600;
		test_a2dp(t1, t2, a2dp_source_aptx_hd, a2dp_sink_aptx_hd);
#endif
	}
	else {
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 60;
		test_a2dp(t1, t2, a2dp_source_aptx_hd, test_io_thread_a2dp_dump_bt);
#if HAVE_APTX_HD_DECODE
		test_a2dp(t1, t2, test_io_thread_a2dp_dump_pcm, a2dp_sink_aptx_hd);
#endif
	};

	ba_transport_destroy(t1);
	ba_transport_destroy(t2);

} END_TEST
#endif

#if ENABLE_LDAC
START_TEST(test_a2dp_ldac) {

	struct ba_transport_type ttype = {
		.profile = BA_TRANSPORT_PROFILE_A2DP_SOURCE,
		.codec = A2DP_CODEC_VENDOR_LDAC };
	struct ba_transport *t1 = ba_transport_new_a2dp(device1, ttype, ":test", "/path/ldac",
			&a2dp_codec_source_ldac, &config_ldac_44100_stereo);
	ttype.profile = BA_TRANSPORT_PROFILE_A2DP_SINK;
	struct ba_transport *t2 = ba_transport_new_a2dp(device2, ttype, ":test", "/path/ldac",
			&a2dp_codec_sink_ldac, &config_ldac_44100_stereo);

	t1->acquire = t2->acquire = test_transport_acquire;
	t1->release = t2->release = test_transport_release_bt_a2dp;

	if (aging_duration) {
#if HAVE_LDAC_DECODE
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write =
			RTP_HEADER_LEN + sizeof(rtp_media_header_t) + 990 + 6;
		test_a2dp(t1, t2, a2dp_source_ldac, a2dp_sink_ldac);
#endif
	}
	else {
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write =
			RTP_HEADER_LEN + sizeof(rtp_media_header_t) + 660 + 6;
		test_a2dp(t1, t2, a2dp_source_ldac, test_io_thread_a2dp_dump_bt);
#if HAVE_LDAC_DECODE
		test_a2dp(t1, t2, test_io_thread_a2dp_dump_pcm, a2dp_sink_ldac);
#endif
	}

	ba_transport_destroy(t1);
	ba_transport_destroy(t2);

} END_TEST
#endif

START_TEST(test_sco_cvsd) {

	struct ba_transport_type ttype = {
		.profile = BA_TRANSPORT_PROFILE_HSP_AG };
	struct ba_transport *t = ba_transport_new_sco(device1, ttype, ":test", "/path/sco/cvsd", -1);

	t->acquire = test_transport_acquire;

	t->mtu_read = t->mtu_write = 48;
	test_sco(t, sco_enc_thread, sco_dec_thread);

	ba_transport_destroy(t);

} END_TEST

#if ENABLE_MSBC
START_TEST(test_sco_msbc) {

	struct ba_transport_type ttype = {
		.profile = BA_TRANSPORT_PROFILE_HFP_AG,
		.codec = HFP_CODEC_MSBC };
	struct ba_transport *t = ba_transport_new_sco(device1, ttype, ":test", "/path/sco/msbc", -1);

	t->acquire = test_transport_acquire;

	t->mtu_read = t->mtu_write = 24;
	test_sco(t, sco_enc_thread, sco_dec_thread);

	ba_transport_destroy(t);

} END_TEST
#endif

int main(int argc, char *argv[]) {

	int opt;
	const char *opts = "h";
	struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "aging", required_argument, NULL, 'a' },
		{ "dump", no_argument, NULL, 'd' },
		{ "input", required_argument, NULL, 'i' },
		{ 0, 0, 0, 0 },
	};

	struct {
		const char *name;
		unsigned int flag;
	} codecs[] = {
#define TEST_CODEC_SBC  (1 << 0)
		{ ba_transport_codecs_a2dp_to_string(A2DP_CODEC_SBC), TEST_CODEC_SBC },
#define TEST_CODEC_MP3  (1 << 1)
		{ ba_transport_codecs_a2dp_to_string(A2DP_CODEC_MPEG12), TEST_CODEC_MP3 },
#define TEST_CODEC_AAC  (1 << 2)
		{ ba_transport_codecs_a2dp_to_string(A2DP_CODEC_MPEG24), TEST_CODEC_AAC },
#define TEST_CODEC_APTX (1 << 3)
		{ ba_transport_codecs_a2dp_to_string(A2DP_CODEC_VENDOR_APTX), TEST_CODEC_APTX },
#define TEST_CODEC_APTX_HD (1 << 4)
		{ ba_transport_codecs_a2dp_to_string(A2DP_CODEC_VENDOR_APTX_HD), TEST_CODEC_APTX_HD },
#define TEST_CODEC_FASTSTREAM (1 << 5)
		{ ba_transport_codecs_a2dp_to_string(A2DP_CODEC_VENDOR_FASTSTREAM), TEST_CODEC_FASTSTREAM },
#define TEST_CODEC_LDAC (1 << 6)
		{ ba_transport_codecs_a2dp_to_string(A2DP_CODEC_VENDOR_LDAC), TEST_CODEC_LDAC },
#define TEST_CODEC_CVSD (1 << 7)
		{ ba_transport_codecs_hfp_to_string(HFP_CODEC_CVSD), TEST_CODEC_CVSD },
#define TEST_CODEC_MSBC (1 << 8)
		{ ba_transport_codecs_hfp_to_string(HFP_CODEC_MSBC), TEST_CODEC_MSBC },
	};

	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1)
		switch (opt) {
		case 'h' /* --help */ :
			printf("usage: %s [--aging=SEC] [--dump] [--input=FILE] [codec ...]\n", argv[0]);
			return 0;
		case 'a' /* --aging=SEC */ :
			aging_duration = atoi(optarg);
			break;
		case 'd' /* --dump */ :
			dump_data = true;
			break;
		case 'i' /* --input=FILE */ :
			input_pcm_file = optarg;
			break;
		default:
			fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
			return 1;
		}

	unsigned int enabled_codecs = 0xFFFF;
	size_t i;

	if (optind != argc)
		enabled_codecs = 0;
	for (; optind < argc; optind++)
		for (i = 0; i < ARRAYSIZE(codecs); i++)
			if (strcasecmp(argv[optind], codecs[i].name) == 0)
				enabled_codecs |= codecs[i].flag;

	bdaddr_t addr1 = {{ 1, 2, 3, 4, 5, 6 }};
	bdaddr_t addr2 = {{ 1, 2, 3, 7, 8, 9 }};
	adapter = ba_adapter_new(0);
	device1 = ba_device_new(adapter, &addr1);
	device2 = ba_device_new(adapter, &addr2);

	Suite *s = suite_create(__FILE__);
	TCase *tc = tcase_create(__FILE__);
	SRunner *sr = srunner_create(s);

	suite_add_tcase(s, tc);
	tcase_set_timeout(tc, aging_duration +
			(input_pcm_file != NULL ? 180 : 5));

	if (enabled_codecs & TEST_CODEC_SBC)
		tcase_add_test(tc, test_a2dp_sbc);
#if ENABLE_MP3LAME
	if (enabled_codecs & TEST_CODEC_MP3)
		tcase_add_test(tc, test_a2dp_mp3);
#endif
#if ENABLE_AAC
	config.aac_afterburner = true;
	if (enabled_codecs & TEST_CODEC_AAC)
		tcase_add_test(tc, test_a2dp_aac);
#endif
#if ENABLE_APTX
	if (enabled_codecs & TEST_CODEC_APTX)
		tcase_add_test(tc, test_a2dp_aptx);
#endif
#if ENABLE_APTX_HD
	if (enabled_codecs & TEST_CODEC_APTX_HD)
		tcase_add_test(tc, test_a2dp_aptx_hd);
#endif
#if ENABLE_LDAC
	config.ldac_abr = true;
	config.ldac_eqmid = LDACBT_EQMID_HQ;
	if (enabled_codecs & TEST_CODEC_LDAC)
		tcase_add_test(tc, test_a2dp_ldac);
#endif
	if (enabled_codecs & TEST_CODEC_CVSD)
		tcase_add_test(tc, test_sco_cvsd);
#if ENABLE_MSBC
	if (enabled_codecs & TEST_CODEC_MSBC)
		tcase_add_test(tc, test_sco_msbc);
#endif

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);
	srunner_free(sr);

	return nf == 0 ? 0 : 1;
}
