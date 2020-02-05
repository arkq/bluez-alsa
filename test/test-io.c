/*
 * test-io.c
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <getopt.h>
#include <sys/sendfile.h>

#include <check.h>

#include "inc/sine.inc"
#include "../src/a2dp.c"
#include "../src/at.c"
#include "../src/ba-adapter.c"
#include "../src/ba-device.c"
#include "../src/ba-transport.c"
#include "../src/bluealsa.c"
#include "../src/hci.c"
#include "../src/msbc.c"
#include "../src/rfcomm.c"
#include "../src/sco.c"
#include "../src/utils.c"
#include "../src/shared/ffb.c"
#include "../src/shared/log.c"
#include "../src/shared/rt.c"

unsigned int bluealsa_dbus_pcm_register(struct ba_transport_pcm *pcm, GError **error) {
	debug("%s: %p", __func__, (void *)pcm); (void)error; return 0; }
void bluealsa_dbus_pcm_update(struct ba_transport_pcm *pcm, unsigned int mask) {
	debug("%s: %p %#x", __func__, (void *)pcm, mask); }
void bluealsa_dbus_pcm_unregister(struct ba_transport_pcm *pcm) {
	debug("%s: %p", __func__, (void *)pcm); }
unsigned int bluealsa_dbus_rfcomm_register(struct ba_transport *t, GError **error) {
	debug("%s: %p", __func__, (void *)t); (void)error; return 0; }
void bluealsa_dbus_rfcomm_update(struct ba_transport *t, unsigned int mask) {
	debug("%s: %p %#x", __func__, (void *)t, mask); }
void bluealsa_dbus_rfcomm_unregister(struct ba_transport *t) {
	debug("%s: %p", __func__, (void *)t); }

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
	.object_type = AAC_OBJECT_TYPE_MPEG4_AAC_LC,
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

/**
 * Write test PCM signal to the file descriptor. */
static void write_test_pcm(int fd, int channels) {

	static int16_t sample_pcm_mono[5 * 1024];
	static int16_t sample_pcm_stereo[10 * 1024];
	static bool initialized = false;
	FILE *f;
	int ffd;

	if (input_pcm_file != NULL) {
		ck_assert_int_ne(ffd = open(input_pcm_file, O_RDONLY), -1);
		ck_assert_int_ne(sendfile(fd, ffd, NULL, 0x7FFFF000), -1);
		close(ffd);
		return;
	}

	if (!initialized) {

		/* initialize build-in sample PCM test signals */
		snd_pcm_sine_s16le(sample_pcm_mono, ARRAYSIZE(sample_pcm_mono), 1, 0, 1.0 / 128);
		snd_pcm_sine_s16le(sample_pcm_stereo, ARRAYSIZE(sample_pcm_stereo), 2, 0, 1.0 / 128);
		initialized = true;

		if (dump_data) {
			ck_assert_ptr_ne(f = fopen("sample-mono.pcm", "w"), NULL);
			fwrite(sample_pcm_mono, 1, sizeof(sample_pcm_mono), f);
			fclose(f);
			ck_assert_ptr_ne(f = fopen("sample-stereo.pcm", "w"), NULL);
			fwrite(sample_pcm_stereo, 1, sizeof(sample_pcm_stereo), f);
			fclose(f);
		}

	}

	switch (channels) {
	case 1:
		ck_assert_int_eq(write(fd, sample_pcm_mono, sizeof(sample_pcm_mono)), sizeof(sample_pcm_mono));
		break;
	case 2:
		ck_assert_int_eq(write(fd, sample_pcm_stereo, sizeof(sample_pcm_stereo)), sizeof(sample_pcm_stereo));
		break;
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

/**
 * Helper function for timed thread join.
 *
 * This function takes the timeout value in microseconds. */
static int pthread_timedjoin(pthread_t thread, void **retval, useconds_t usec) {

	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_nsec += (long)usec * 1000;

	/* normalize timespec structure */
	ts.tv_sec += ts.tv_nsec / (long)1e9;
	ts.tv_nsec = ts.tv_nsec % (long)1e9;

	return pthread_timedjoin_np(thread, retval, &ts);
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

static void *test_io_thread_a2dp_dump_bt(struct ba_transport *t) {

	struct pollfd pfds[] = {{ t->bt_fd, POLLIN, 0 }};
	uint8_t buffer[1024];
	ssize_t len;

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

	/* signal termination and wait for cancelation */
	test_a2dp_start_terminate_timer(0);
	sleep(3600);
	return NULL;
}

static void *test_io_thread_a2dp_dump_pcm(struct ba_transport *t) {

	struct pollfd pfds[] = {{ t->a2dp.pcm.fd, POLLIN, 0 }};
	size_t decoded_samples_total = 0;
	int16_t buffer[2048];
	ssize_t len;
	char fname[64];
	FILE *f = NULL;

	if (dump_data) {
		sprintf(fname, "decoded-%s.pcm", bluetooth_a2dp_codec_to_string(t->type.codec));
		ck_assert_ptr_ne(f = fopen(fname, "w"), NULL);
	}

	debug("Starting PCM dump: %d", pfds[0].fd);
	while (poll(pfds, ARRAYSIZE(pfds), 500) > 0) {

		if ((len = read(pfds[0].fd, buffer, sizeof(buffer))) == -1) {
			debug("PCM read error: %s", strerror(errno));
			continue;
		}

		debug("Decoded samples: %zd", len / sizeof(int16_t));
		decoded_samples_total += len / sizeof(int16_t);

		if (f != NULL)
			fwrite(buffer, 1, len, f);
	}

	debug("Decoded samples total: %zd", decoded_samples_total);

	if (f != NULL)
		fclose(f);

	/* signal termination and wait for cancelation */
	test_a2dp_start_terminate_timer(0);
	sleep(3600);
	return NULL;
}

/**
 * Drive PCM signal through A2DP source/sink loop. */
static void test_a2dp(struct ba_transport *t1, struct ba_transport *t2,
		void *(*enc)(struct ba_transport *), void *(*dec)(struct ba_transport *)) {

	int bt_fds[2];
	int pcm_fds[2];

	ck_assert_int_eq(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, bt_fds), 0);
	ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pcm_fds), 0);

	if (dec == test_io_thread_a2dp_dump_bt)
		bt_data_init();

	t1->type.profile = BA_TRANSPORT_PROFILE_A2DP_SOURCE;
	t2->type.profile = BA_TRANSPORT_PROFILE_A2DP_SINK;
	t1->state = BA_TRANSPORT_STATE_ACTIVE;
	t2->state = BA_TRANSPORT_STATE_ACTIVE;
	t1->bt_fd = bt_fds[1];
	t2->bt_fd = bt_fds[0];
	t1->a2dp.pcm.fd = pcm_fds[1];
	t2->a2dp.pcm.fd = pcm_fds[0];

	pthread_t thread1;
	pthread_t thread2;

	if (aging_duration)
		test_a2dp_start_terminate_timer(aging_duration);

	if (enc == test_io_thread_a2dp_dump_pcm) {
		pthread_create(&thread2, NULL, PTHREAD_ROUTINE(dec), ba_transport_ref(t2));
		struct bt_data *bt_data_head = &bt_data;
		for (; bt_data_head != bt_data_end; bt_data_head = bt_data_head->next)
			ck_assert_int_eq(write(bt_fds[1], bt_data_head->data, bt_data_head->len), bt_data_head->len);
		pthread_create(&thread1, NULL, PTHREAD_ROUTINE(enc), ba_transport_ref(t1));
	}
	else {
		pthread_create(&thread1, NULL, PTHREAD_ROUTINE(enc), ba_transport_ref(t1));
		write_test_pcm(pcm_fds[0], t1->a2dp.pcm.channels);
		pthread_create(&thread2, NULL, PTHREAD_ROUTINE(dec), ba_transport_ref(t2));
	}

	pthread_mutex_lock(&test_a2dp_mutex);
	pthread_cond_wait(&test_a2dp_terminate, &test_a2dp_mutex);
	pthread_mutex_unlock(&test_a2dp_mutex);

	ck_assert_int_eq(pthread_cancel(thread1), 0);
	ck_assert_int_eq(pthread_cancel(thread2), 0);

	ck_assert_int_eq(pthread_timedjoin(thread1, NULL, 1e6), 0);
	ck_assert_int_eq(pthread_timedjoin(thread2, NULL, 1e6), 0);

}

static void test_sco(struct ba_transport *t, void *(*cb)(struct ba_transport *)) {

	int sco_fds[2];
	int pcm_mic_fds[2];
	int pcm_spk_fds[2];

	ck_assert_int_eq(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sco_fds), 0);
	ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM, 0, pcm_mic_fds), 0);
	ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM, 0, pcm_spk_fds), 0);
	write_test_pcm(pcm_spk_fds[0], t->sco.spk_pcm.channels);

	t->state = BA_TRANSPORT_STATE_ACTIVE;
	t->bt_fd = sco_fds[1];
	t->sco.mic_pcm.fd = pcm_mic_fds[1];
	t->sco.spk_pcm.fd = pcm_spk_fds[1];

	pthread_t thread;
	pthread_create(&thread, NULL, PTHREAD_ROUTINE(cb), ba_transport_ref(t));

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
			debug("Decoded samples: %zd", len / sizeof(int16_t));
			decoded_samples_total += len / sizeof(int16_t);
		}

	}

	debug("Decoded samples total: %zd", decoded_samples_total);

	ck_assert_int_eq(pthread_cancel(thread), 0);
	ck_assert_int_eq(pthread_timedjoin(thread, NULL, 1e6), 0);

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
			&config_sbc_44100_stereo, sizeof(config_sbc_44100_stereo));
	struct ba_transport *t2 = ba_transport_new_a2dp(device2, ttype, ":test", "/path/sbc",
			&config_sbc_44100_stereo, sizeof(config_sbc_44100_stereo));

	t1->acquire = t2->acquire = test_transport_acquire;
	t1->release = t2->release = test_transport_release_bt_a2dp;

	if (aging_duration) {
		t1->mtu_write = t2->mtu_read = 153 * 3;
		test_a2dp(t1, t2, a2dp_source_sbc, a2dp_sink_sbc);
	}
	else {
		t1->mtu_write = t2->mtu_read = 153 * 3;
		test_a2dp(t1, t2, a2dp_source_sbc, test_io_thread_a2dp_dump_bt);
		test_a2dp(t1, t2, test_io_thread_a2dp_dump_pcm, a2dp_sink_sbc);
	}

} END_TEST

#if ENABLE_MP3LAME
START_TEST(test_a2dp_mp3) {

	struct ba_transport_type ttype = {
		.profile = BA_TRANSPORT_PROFILE_A2DP_SOURCE,
		.codec = A2DP_CODEC_MPEG12 };
	struct ba_transport *t1 = ba_transport_new_a2dp(device1, ttype, ":test", "/path/mp3",
			&config_mp3_44100_stereo, sizeof(config_mp3_44100_stereo));
	struct ba_transport *t2 = ba_transport_new_a2dp(device2, ttype, ":test", "/path/mp3",
			&config_mp3_44100_stereo, sizeof(config_mp3_44100_stereo));

	t1->acquire = t2->acquire = test_transport_acquire;
	t1->release = t2->release = test_transport_release_bt_a2dp;

	if (aging_duration) {
		t1->mtu_write = t2->mtu_read = 1024;
		test_a2dp(t1, t2, a2dp_source_mp3, a2dp_sink_mpeg);
	}
	else {
		t1->mtu_write = t2->mtu_read = 250;
		test_a2dp(t1, t2, a2dp_source_mp3, test_io_thread_a2dp_dump_bt);
		test_a2dp(t1, t2, test_io_thread_a2dp_dump_pcm, a2dp_sink_mpeg);
	}

} END_TEST
#endif

#if ENABLE_AAC
START_TEST(test_a2dp_aac) {

	struct ba_transport_type ttype = {
		.profile = BA_TRANSPORT_PROFILE_A2DP_SOURCE,
		.codec = A2DP_CODEC_MPEG24 };
	struct ba_transport *t1 = ba_transport_new_a2dp(device1, ttype, ":test", "/path/aac",
			&config_aac_44100_stereo, sizeof(config_aac_44100_stereo));
	struct ba_transport *t2 = ba_transport_new_a2dp(device2, ttype, ":test", "/path/aac",
			&config_aac_44100_stereo, sizeof(config_aac_44100_stereo));

	t1->acquire = t2->acquire = test_transport_acquire;
	t1->release = t2->release = test_transport_release_bt_a2dp;

	if (aging_duration) {
		t1->mtu_write = t2->mtu_read = 450;
		test_a2dp(t1, t2, a2dp_source_aac, a2dp_sink_aac);
	}
	else {
		t1->mtu_write = t2->mtu_read = 64;
		test_a2dp(t1, t2, a2dp_source_aac, test_io_thread_a2dp_dump_bt);
		test_a2dp(t1, t2, test_io_thread_a2dp_dump_pcm, a2dp_sink_aac);
	}

} END_TEST
#endif

#if ENABLE_APTX
START_TEST(test_a2dp_aptx) {

	struct ba_transport_type ttype = {
		.profile = BA_TRANSPORT_PROFILE_A2DP_SOURCE,
		.codec = A2DP_CODEC_VENDOR_APTX };
	struct ba_transport *t1 = ba_transport_new_a2dp(device1, ttype, ":test", "/path/aptx",
			&config_aptx_44100_stereo, sizeof(config_aptx_44100_stereo));
	struct ba_transport *t2 = ba_transport_new_a2dp(device1, ttype, ":test", "/path/aptx",
			&config_aptx_44100_stereo, sizeof(config_aptx_44100_stereo));

	t1->acquire = t2->acquire = test_transport_acquire;
	t1->release = t2->release = test_transport_release_bt_a2dp;

	if (aging_duration) {
	}
	else {
		t1->mtu_write = t2->mtu_read = 40;
		test_a2dp(t1, t2, a2dp_source_aptx, test_io_thread_a2dp_dump_bt);
	};

} END_TEST
#endif

#if ENABLE_APTX_HD
START_TEST(test_a2dp_aptx_hd) {

	struct ba_transport_type ttype = {
		.profile = BA_TRANSPORT_PROFILE_A2DP_SOURCE,
		.codec = A2DP_CODEC_VENDOR_APTX_HD };
	struct ba_transport *t1 = ba_transport_new_a2dp(device1, ttype, ":test", "/path/aptxhd",
			&config_aptx_hd_44100_stereo, sizeof(config_aptx_hd_44100_stereo));
	struct ba_transport *t2 = ba_transport_new_a2dp(device1, ttype, ":test", "/path/aptxhd",
			&config_aptx_hd_44100_stereo, sizeof(config_aptx_hd_44100_stereo));

	t1->acquire = t2->acquire = test_transport_acquire;
	t1->release = t2->release = test_transport_release_bt_a2dp;

	if (aging_duration) {
	}
	else {
		t1->mtu_write = t2->mtu_read = 60;
		test_a2dp(t1, t2, a2dp_source_aptx_hd, test_io_thread_a2dp_dump_bt);
	};

} END_TEST
#endif

#if ENABLE_LDAC
START_TEST(test_a2dp_ldac) {

	struct ba_transport_type ttype = {
		.profile = BA_TRANSPORT_PROFILE_A2DP_SOURCE,
		.codec = A2DP_CODEC_VENDOR_LDAC };
	struct ba_transport *t1 = ba_transport_new_a2dp(device1, ttype, ":test", "/path/ldac",
			&config_ldac_44100_stereo, sizeof(config_ldac_44100_stereo));
	struct ba_transport *t2 = ba_transport_new_a2dp(device1, ttype, ":test", "/path/ldac",
			&config_ldac_44100_stereo, sizeof(config_ldac_44100_stereo));

	t1->acquire = t2->acquire = test_transport_acquire;
	t1->release = t2->release = test_transport_release_bt_a2dp;

	if (aging_duration) {
	}
	else {
		t1->mtu_write = t2->mtu_read = RTP_HEADER_LEN + sizeof(rtp_media_header_t) + 679;
		test_a2dp(t1, t2, a2dp_source_ldac, test_io_thread_a2dp_dump_bt);
	}

} END_TEST
#endif

START_TEST(test_sco_cvsd) {

	struct ba_transport_type ttype = { .profile = BA_TRANSPORT_PROFILE_HSP_AG };
	struct ba_transport *t = ba_transport_new_sco(device1, ttype, ":test", "/path/sco/cvsd", NULL);

	t->mtu_read = t->mtu_write = 48;
	t->acquire = test_transport_acquire;

	ba_transport_send_signal(t, BA_TRANSPORT_SIGNAL_PING);
	test_sco(t, sco_thread);

} END_TEST

#if ENABLE_MSBC
START_TEST(test_sco_msbc) {

	struct ba_transport_type ttype = {
		.profile = BA_TRANSPORT_PROFILE_HFP_AG,
		.codec = HFP_CODEC_MSBC };
	struct ba_transport *t = ba_transport_new_sco(device1, ttype, ":test", "/path/sco/msbc", NULL);

	t->mtu_read = t->mtu_write = 24;
	t->acquire = test_transport_acquire;

	ba_transport_send_signal(t, BA_TRANSPORT_SIGNAL_PING);
	test_sco(t, sco_thread);

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
		{ "SBC", TEST_CODEC_SBC },
#define TEST_CODEC_MP3  (1 << 1)
		{ "MP3", TEST_CODEC_MP3 },
#define TEST_CODEC_AAC  (1 << 2)
		{ "AAC", TEST_CODEC_AAC },
#define TEST_CODEC_APTX (1 << 3)
		{ "APTX", TEST_CODEC_APTX },
#define TEST_CODEC_APTX_HD (1 << 4)
		{ "APTXHD", TEST_CODEC_APTX_HD },
#define TEST_CODEC_LDAC (1 << 5)
		{ "LDAC", TEST_CODEC_LDAC },
#define TEST_CODEC_CVSD (1 << 6)
		{ "CVSD", TEST_CODEC_CVSD },
#define TEST_CODEC_MSBC (1 << 7)
		{ "mSBC", TEST_CODEC_MSBC },
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
	tcase_set_timeout(tc, aging_duration + 5);

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
