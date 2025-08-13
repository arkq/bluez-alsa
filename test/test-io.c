/*
 * test-io.c
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#if HAVE_CONFIG_H
# include <config.h>
# define ENABLE_APTX_IO_TEST    (ENABLE_APTX && HAVE_APTX_DECODE)
# define ENABLE_APTX_HD_IO_TEST (ENABLE_APTX_HD && HAVE_APTX_HD_DECODE)
# define ENABLE_LDAC_IO_TEST    (ENABLE_LDAC && HAVE_LDAC_DECODE)
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
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <check.h>
#include <glib.h>
#if ENABLE_LDAC_IO_TEST
# include <ldacBT.h>
#endif
#if ENABLE_LHDC
# include <lhdcBT.h>
#endif
#if HAVE_SNDFILE
# include <sndfile.h>
#endif

#include "a2dp.h"
#if ENABLE_AAC
# include "a2dp-aac.h"
#endif
#if ENABLE_APTX_IO_TEST
# include "a2dp-aptx.h"
#endif
#if ENABLE_APTX_HD_IO_TEST
# include "a2dp-aptx-hd.h"
#endif
#if ENABLE_FASTSTREAM
# include "a2dp-faststream.h"
#endif
#if ENABLE_LC3PLUS
# include "a2dp-lc3plus.h"
#endif
#if ENABLE_LDAC_IO_TEST
# include "a2dp-ldac.h"
#endif
#if ENABLE_LHDC
# include "a2dp-lhdc.h"
#endif
#if ENABLE_MPEG
# include "a2dp-mpeg.h"
#endif
#if ENABLE_OPUS
# include "a2dp-opus.h"
#endif
#include "a2dp-sbc.h"
#include "ba-adapter.h"
#include "ba-config.h"
#include "ba-device.h"
#include "ba-rfcomm.h"
#include "ba-transport.h"
#include "ba-transport-pcm.h"
#include "ble-midi.h"
#include "bluealsa-dbus.h"
#include "bluez.h"
#include "hfp.h"
#include "io.h"
#include "midi.h"
#if ENABLE_OFONO
# include "ofono.h"
#endif
#if ENABLE_LC3PLUS || ENABLE_LDAC_IO_TEST
# include "rtp.h"
#endif
#include "storage.h"
#include "shared/a2dp-codecs.h"
#include "shared/defs.h"
#include "shared/log.h"

#include "../src/a2dp.c"
#include "../src/ba-transport.c"
#include "inc/btd.inc"
#include "inc/check.inc"
#include "inc/sine.inc"

#define CHECK_VERSION ( \
		(CHECK_MAJOR_VERSION << 16 & 0xff0000) | \
		(CHECK_MINOR_VERSION << 8 & 0x00ff00) | \
		(CHECK_MICRO_VERSION << 0 & 0x0000ff))

void *a2dp_aac_dec_thread(struct ba_transport_pcm *t_pcm);
void *a2dp_aac_enc_thread(struct ba_transport_pcm *t_pcm);
void *a2dp_aptx_dec_thread(struct ba_transport_pcm *t_pcm);
void *a2dp_aptx_enc_thread(struct ba_transport_pcm *t_pcm);
void *a2dp_aptx_hd_dec_thread(struct ba_transport_pcm *t_pcm);
void *a2dp_aptx_hd_enc_thread(struct ba_transport_pcm *t_pcm);
void *a2dp_fs_dec_thread(struct ba_transport_pcm *t_pcm);
void *a2dp_fs_enc_thread(struct ba_transport_pcm *t_pcm);
void *a2dp_lc3plus_dec_thread(struct ba_transport_pcm *t_pcm);
void *a2dp_lc3plus_enc_thread(struct ba_transport_pcm *t_pcm);
void *a2dp_ldac_dec_thread(struct ba_transport_pcm *t_pcm);
void *a2dp_ldac_enc_thread(struct ba_transport_pcm *t_pcm);
void *a2dp_lhdc_dec_thread(struct ba_transport_pcm *t_pcm);
void *a2dp_lhdc_enc_thread(struct ba_transport_pcm *t_pcm);
void *a2dp_mp3_enc_thread(struct ba_transport_pcm *t_pcm);
void *a2dp_mpeg_dec_thread(struct ba_transport_pcm *t_pcm);
void *a2dp_opus_dec_thread(struct ba_transport_pcm *t_pcm);
void *a2dp_opus_enc_thread(struct ba_transport_pcm *t_pcm);
void *a2dp_sbc_dec_thread(struct ba_transport_pcm *t_pcm);
void *a2dp_sbc_enc_thread(struct ba_transport_pcm *t_pcm);
void *sco_dec_thread(struct ba_transport_pcm *t_pcm);
void *sco_enc_thread(struct ba_transport_pcm *t_pcm);

int bluealsa_dbus_pcm_register(struct ba_transport_pcm *pcm) {
	debug("%s: %p", __func__, (void *)pcm); (void)pcm; return 0; }
void bluealsa_dbus_pcm_update(struct ba_transport_pcm *pcm, unsigned int mask) {
	debug("%s: %p %#x", __func__, (void *)pcm, mask); (void)pcm; (void)mask; }
void bluealsa_dbus_pcm_unregister(struct ba_transport_pcm *pcm) {
	debug("%s: %p", __func__, (void *)pcm); (void)pcm; }
struct ba_rfcomm *ba_rfcomm_new(struct ba_transport *sco, int fd) {
	debug("%s: %p", __func__, (void *)sco); (void)sco; (void)fd; return NULL; }
void ba_rfcomm_destroy(struct ba_rfcomm *r) {
	debug("%s: %p", __func__, (void *)r); (void)r; }
int ba_rfcomm_send_signal(struct ba_rfcomm *r, enum ba_rfcomm_signal sig) {
	debug("%s: %p: %#x", __func__, (void *)r, sig); (void)r; (void)sig; return 0; }
bool bluez_a2dp_set_configuration(const char *current_dbus_sep_path,
		const struct a2dp_sep_config *sep, const void *configuration, GError **error) {
	debug("%s: %s: %p", __func__, current_dbus_sep_path, sep);
	(void)current_dbus_sep_path; (void)sep; (void)configuration; (void)error;
	return false; }
int ofono_call_volume_update(struct ba_transport *t) {
	debug("%s: %p", __func__, t); (void)t; return 0; }
void ble_midi_decode_free(struct ble_midi_dec *bmd) { (void)bmd; }
int midi_transport_alsa_seq_create(struct ba_transport *t) { (void)t; return 0; }
int midi_transport_alsa_seq_delete(struct ba_transport *t) { (void)t; return 0; }
int midi_transport_start(struct ba_transport *t) { (void)t; return 0; }
int midi_transport_stop(struct ba_transport *t) { (void)t; return 0; }
int storage_device_load(const struct ba_device *d) { (void)d; return 0; }
int storage_device_save(const struct ba_device *d) { (void)d; return 0; }
int storage_pcm_data_sync(struct ba_transport_pcm *pcm) { (void)pcm; return 0; }
int storage_pcm_data_update(const struct ba_transport_pcm *pcm) { (void)pcm; return 0; }

static const a2dp_sbc_t config_sbc_44100_stereo = {
	.sampling_freq = SBC_SAMPLING_FREQ_44100,
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
	.sampling_freq = MPEG_SAMPLING_FREQ_44100,
	A2DP_MPEG_INIT_BITRATE(0xFFFF)
};

__attribute__ ((unused))
static a2dp_aac_t config_aac_44100_stereo = {
	.object_type = AAC_OBJECT_TYPE_MPEG2_LC,
	A2DP_AAC_INIT_SAMPLING_FREQ(AAC_SAMPLING_FREQ_44100)
	.channel_mode = AAC_CHANNEL_MODE_STEREO,
	A2DP_AAC_INIT_BITRATE(0xFFFF)
};

__attribute__ ((unused))
static const a2dp_aptx_t config_aptx_44100_stereo = {
	.info = A2DP_VENDOR_INFO_INIT(APTX_VENDOR_ID, APTX_CODEC_ID),
	.sampling_freq = APTX_SAMPLING_FREQ_44100,
	.channel_mode = APTX_CHANNEL_MODE_STEREO,
};

__attribute__ ((unused))
static const a2dp_aptx_hd_t config_aptx_hd_44100_stereo = {
	.aptx.info = A2DP_VENDOR_INFO_INIT(APTX_HD_VENDOR_ID, APTX_HD_CODEC_ID),
	.aptx.sampling_freq = APTX_SAMPLING_FREQ_44100,
	.aptx.channel_mode = APTX_CHANNEL_MODE_STEREO,
};

__attribute__ ((unused))
static const a2dp_faststream_t config_faststream_44100_16000 = {
	.info = A2DP_VENDOR_INFO_INIT(FASTSTREAM_VENDOR_ID, FASTSTREAM_CODEC_ID),
	.direction = FASTSTREAM_DIRECTION_MUSIC | FASTSTREAM_DIRECTION_VOICE,
	.sampling_freq_music = FASTSTREAM_SAMPLING_FREQ_MUSIC_44100,
	.sampling_freq_voice = FASTSTREAM_SAMPLING_FREQ_VOICE_16000,
};

__attribute__ ((unused))
static const a2dp_lc3plus_t config_lc3plus_48000_stereo = {
	.info = A2DP_VENDOR_INFO_INIT(LC3PLUS_VENDOR_ID, LC3PLUS_CODEC_ID),
	.frame_duration = LC3PLUS_FRAME_DURATION_050,
	.channel_mode = LC3PLUS_CHANNEL_MODE_STEREO,
	A2DP_LC3PLUS_INIT_SAMPLING_FREQ(LC3PLUS_SAMPLING_FREQ_48000)
};

__attribute__ ((unused))
static const a2dp_ldac_t config_ldac_48000_stereo = {
	.info = A2DP_VENDOR_INFO_INIT(LDAC_VENDOR_ID, LDAC_CODEC_ID),
	.sampling_freq = LDAC_SAMPLING_FREQ_48000,
	.channel_mode = LDAC_CHANNEL_MODE_STEREO,
};

__attribute__ ((unused))
static const a2dp_lhdc_v3_t config_lhdc_v3_48000_stereo = {
	.info = A2DP_VENDOR_INFO_INIT(LHDC_V3_VENDOR_ID, LHDC_V3_CODEC_ID),
	.sampling_freq = LHDC_SAMPLING_FREQ_48000,
	.bit_depth = LHDC_BIT_DEPTH_24,
};

__attribute__ ((unused))
static const a2dp_opus_t config_opus_48000_stereo = {
	.sampling_freq = OPUS_SAMPLING_FREQ_48000,
	.frame_duration = OPUS_FRAME_DURATION_100,
	.channel_mode = OPUS_CHANNEL_MODE_STEREO,
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

/* input BT dump file */
static struct bt_dump *btdin = NULL;

#if HAVE_SNDFILE
static SNDFILE *sf_open_format(const char *path, unsigned int rate,
		unsigned int channels, uint16_t format) {

	SF_INFO sf_info = {
		.format = SF_FORMAT_WAV,
		.channels = channels,
		.samplerate = rate,
	};

	switch (BA_TRANSPORT_PCM_FORMAT_WIDTH(format)) {
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

	return sf_open(path, SFM_WRITE, &sf_info);
}
#endif

#if HAVE_SNDFILE
static sf_count_t sf_write_format(SNDFILE *sf, const void *buffer,
		sf_count_t samples, uint16_t format) {
	switch (BA_TRANSPORT_PCM_FORMAT_BYTES(format)) {
	case 2:
		return sf_write_short(sf, buffer, samples);
	case 4:
		if (BA_TRANSPORT_PCM_FORMAT_WIDTH(format) == 24)
			for (size_t i = 0; i < (size_t)samples; i++)
				((int *)buffer)[i] <<= 8;
		return sf_write_int(sf, buffer, samples);
	default:
		g_assert_not_reached();
	}
}
#endif

#if HAVE_SNDFILE
static void *pcm_write_frames_sndfile_async(void *userdata) {

	struct ba_transport_pcm *pcm = userdata;
	struct pollfd pfds[] = {{ pcm->fd, POLLOUT, 0 }};

	SNDFILE *sf = NULL;
	SF_INFO sf_info = { 0 };
	if ((sf = sf_open(input_pcm_file, SFM_READ, &sf_info)) == NULL) {
		error("Couldn't load input audio file: %s", sf_strerror(NULL));
		ck_assert_ptr_nonnull(sf);
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
		/* If we are not performing aging test, close the PCM right
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
		int16_t s16[2 /* max channels */ * 1024];
		int32_t s32[2 /* max channels */ * 1024];
	} pcm_sine;

	const size_t pcm_format_bytes = BA_TRANSPORT_PCM_FORMAT_BYTES(pcm->format);
	const size_t pcm_sine_frames = 1024;
	const size_t pcm_sine_samples = pcm->channels * pcm_sine_frames;
	const size_t pcm_sine_bytes = pcm_sine_samples * pcm_format_bytes;
#if HAVE_SNDFILE
	SNDFILE *sf = NULL;
#endif

	switch (pcm->format) {
	case BA_TRANSPORT_PCM_FORMAT_S16_2LE:
		g_assert_cmpuint(pcm_sine_bytes, <=, sizeof(pcm_sine.s16));
		snd_pcm_sine_s16_2le(pcm_sine.s16, pcm->channels, pcm_sine_frames, 1.0 / 128, 0);
		break;
	case BA_TRANSPORT_PCM_FORMAT_S24_4LE:
		g_assert_cmpuint(pcm_sine_bytes, <=, sizeof(pcm_sine.s32));
		snd_pcm_sine_s24_4le(pcm_sine.s32, pcm->channels, pcm_sine_frames, 1.0 / 128, 0);
		break;
	case BA_TRANSPORT_PCM_FORMAT_S32_4LE:
		g_assert_cmpuint(pcm_sine_bytes, <=, sizeof(pcm_sine.s32));
		snd_pcm_sine_s32_4le(pcm_sine.s32, pcm->channels, pcm_sine_frames, 1.0 / 128, 0);
		break;
	default:
		g_assert_not_reached();
	}

	size_t samples = pcm->channels * frames;
	debug("PCM write samples: %zu", samples);

	if (dump_data) {
#if HAVE_SNDFILE
		char fname[128];
		sprintf(fname, "sine-%s.wav", transport_pcm_to_fname(pcm));
		ck_assert_ptr_nonnull(sf = sf_open_format(fname, pcm->rate, pcm->channels, pcm->format));
#else
		error("Dumping audio files requires sndfile library!");
#endif
	}

	int8_t buffer[sizeof(pcm_sine.s32)];
	float fade = 1.0;

	while (samples > 0) {
		size_t x = samples > pcm_sine_samples ? pcm_sine_samples : samples;
		size_t x_bytes = x * pcm_format_bytes;
		samples -= x;

		/* Fade-out the audio signal, so it will be easier to compare
		 * the original signal with the processed one. */
		switch (pcm_format_bytes) {
		case 2:
			for (size_t i = 0; i < x; i++)
				((int16_t *)buffer)[i] = pcm_sine.s16[i] * (fade *= 0.9995);
			break;
		case 4:
			for (size_t i = 0; i < x; i++)
				((int32_t *)buffer)[i] = pcm_sine.s32[i] * (fade *= 0.9995);
			break;
		}

		ck_assert_int_eq(write(pcm->fd, buffer, x_bytes), x_bytes);

#if HAVE_SNDFILE
		if (sf != NULL)
			ck_assert_int_eq(sf_write_format(sf, buffer, x, pcm->format), x);
#endif

	}

	if (aging_duration == 0) {
		/* If we are not performing aging test, close the PCM right
		 * away. The reading loop will not wait for timeout. */
		pthread_mutex_lock(&pcm->mutex);
		ba_transport_pcm_release(pcm);
		pthread_mutex_unlock(&pcm->mutex);
	}

#if HAVE_SNDFILE
	if (sf != NULL)
		sf_close(sf);
#endif

}

struct bt_data {
	struct bt_data *next;
	uint8_t data[2048];
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

		while ((len = bt_dump_read(btdin, buffer, sizeof(buffer))) != -1) {
			if (packet_loss && random() < INT32_MAX / 3 && !first_packet) {
				debug("Simulating packet loss: Dropping BT packet!");
				continue;
			}
			ck_assert_int_ne(poll(fds, ARRAYSIZE(fds), -1), -1);
			ck_assert_int_eq(write(fds[0].fd, buffer, len), len);
			first_packet = false;
			/* Allow the test_io_thread_dump_pcm() to read out the decoded PCM
			 * frames. Otherwise, the PCM FIFO might get full and the decoder
			 * will detect an overrun. In such case, the decoder will drop the
			 * PCM frames and we will not decode the whole BT dump. */
			usleep(500);
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
			usleep(500);
		}

	}

}

static pthread_cond_t test_terminate = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t test_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool test_terminated;

static void *test_terminate_timer(void *arg) {
	sleep((uintptr_t)arg);
	pthread_mutex_lock(&test_mutex);
	test_terminated = true;
	pthread_mutex_unlock(&test_mutex);
	pthread_cond_signal(&test_terminate);
	return NULL;
}

static void test_start_terminate_timer(unsigned int delay) {
	pthread_t thread;
	pthread_create(&thread, NULL, test_terminate_timer, (void *)(uintptr_t)delay);
	pthread_detach(thread);
}

static void *test_io_thread_dump_bt(struct ba_transport_pcm *t_pcm) {

	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pcm_thread_cleanup), t_pcm);

	struct ba_transport *t = t_pcm->t;
	struct pollfd pfds[] = {{ t_pcm->fd_bt, POLLIN, 0 }};
	struct bt_dump *btd = NULL;
	uint8_t buffer[sizeof(bt_data.data)];
	ssize_t len;

	if (dump_data) {
		char fname[128];
		sprintf(fname, "encoded-%s-%s.btd", transport_to_fname(t), transport_pcm_to_fname(t_pcm));
		ck_assert_ptr_nonnull(btd = bt_dump_create(fname, t));
	}

	debug_transport_pcm_thread_loop(t_pcm, "START");
	ba_transport_pcm_state_set_running(t_pcm);
	while (poll(pfds, ARRAYSIZE(pfds), 500) > 0) {

		if ((len = io_bt_read(t_pcm, buffer, sizeof(buffer))) <= 0) {
			if (len == -1)
				error("BT read error: %s", strerror(errno));
			break;
		}

		bt_data_push(buffer, len);
		hexdump("BT data", buffer, len);

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

static void *test_io_thread_dump_pcm(struct ba_transport_pcm *t_pcm) {

	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pcm_thread_cleanup), t_pcm);

	const size_t pcm_format_bytes = BA_TRANSPORT_PCM_FORMAT_BYTES(t_pcm->format);
	size_t decoded_samples_total = 0;

#if HAVE_SNDFILE
	SNDFILE *sf = NULL;
#endif

	if (dump_data) {
#if HAVE_SNDFILE
		char fname[128];
		sprintf(fname, "decoded-%s-%s.wav", transport_to_fname(t_pcm->t), transport_pcm_to_fname(t_pcm));
		ck_assert_ptr_nonnull(sf = sf_open_format(fname, t_pcm->rate, t_pcm->channels, t_pcm->format));
#else
		error("Dumping audio files requires sndfile library!");
#endif
	}

	debug_transport_pcm_thread_loop(t_pcm, "START");
	for (ba_transport_pcm_state_set_running(t_pcm);;) {

		struct pollfd pfds[] = {{ -1, POLLIN, 0 }};
		uint8_t buffer[2048];
		ssize_t len;

		pthread_mutex_lock(&t_pcm->mutex);
		pfds[0].fd = t_pcm->fd;
		pthread_mutex_unlock(&t_pcm->mutex);

		if (poll(pfds, ARRAYSIZE(pfds), 500) <= 0)
			break;

		if ((len = read(pfds[0].fd, buffer, sizeof(buffer))) == -1) {
			debug("PCM read error: %s", strerror(errno));
			continue;
		}

		size_t samples = len / pcm_format_bytes;
		debug("Decoded samples: %zd", samples);
		decoded_samples_total += samples;

#if HAVE_SNDFILE
		if (sf != NULL)
			sf_write_format(sf, buffer, samples, t_pcm->format);
#endif

	}

	debug("Decoded samples total [%zu frames]: %zu",
			decoded_samples_total / t_pcm->channels, decoded_samples_total);
	ck_assert_uint_gt(decoded_samples_total, 0);

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
static void test_io(
		struct ba_transport_pcm *t_src_pcm, struct ba_transport_pcm *t_snk_pcm,
		ba_transport_pcm_thread_func enc, ba_transport_pcm_thread_func dec,
		size_t pcm_write_frames_count) {

	const char *enc_name = enc == test_io_thread_dump_pcm ? "dump-pcm" : "encode";
	const char *dec_name = dec == test_io_thread_dump_bt ? "dump-bt" : "decode";
	struct ba_transport *t_src = t_src_pcm->t;
	struct ba_transport *t_snk = t_snk_pcm->t;

	if (enc == test_io_thread_dump_pcm && input_pcm_file != NULL)
		return;
	if (dec == test_io_thread_dump_bt && input_bt_file != NULL)
		return;

	/* Reset the global termination flag before starting the loop. */
	test_terminated = false;

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
		ck_assert_int_eq(ba_transport_pcm_start(t_snk_pcm, dec, dec_name), 0);
		ck_assert_int_eq(ba_transport_pcm_start(t_src_pcm, enc, enc_name), 0);
		bt_data_write(t_src);
	}
	else {
		ck_assert_int_eq(ba_transport_pcm_start(t_src_pcm, enc, enc_name), 0);
		ck_assert_int_eq(ba_transport_pcm_start(t_snk_pcm, dec, dec_name), 0);
		pcm_write_frames(t_snk_pcm, pcm_write_frames_count);
	}

	pthread_mutex_lock(&test_mutex);
	while (!test_terminated)
		pthread_cond_wait(&test_terminate, &test_mutex);
	pthread_mutex_unlock(&test_mutex);

	pthread_mutex_lock(&t_src_pcm->mutex);
	ba_transport_pcm_release(t_src_pcm);
	pthread_mutex_unlock(&t_src_pcm->mutex);

	ba_transport_stop(t_src);

	pthread_mutex_lock(&t_snk_pcm->mutex);
	ba_transport_pcm_release(t_snk_pcm);
	pthread_mutex_unlock(&t_snk_pcm->mutex);

	ba_transport_stop(t_snk);

}

static int test_transport_acquire(struct ba_transport *t) {
	debug("Acquire transport: %d", t->bt_fd); (void)t;
	return 0;
}

static int test_transport_release_bt_a2dp(struct ba_transport *t) {
	free(t->bluez_dbus_owner); t->bluez_dbus_owner = NULL;
	return transport_release_bt_a2dp(t);
}

static struct ba_transport *test_transport_new_a2dp(
		struct ba_device *device,
		enum ba_transport_profile profile,
		const char *dbus_path,
		const struct a2dp_sep *sep,
		const void *configuration) {
#if DEBUG
	if (input_bt_file != NULL)
		configuration = &btdin->a2dp_configuration;
#endif
	struct ba_transport *t;
	ck_assert_ptr_nonnull(t = ba_transport_new_a2dp(device, profile, ":test",
				dbus_path, sep, configuration));
	t->acquire = test_transport_acquire;
	t->release = test_transport_release_bt_a2dp;
	return t;
}

static struct ba_transport *test_transport_new_sco(
		struct ba_device *device,
		enum ba_transport_profile profile,
		const char *dbus_path) {
	struct ba_transport *t = ba_transport_new_sco(device, profile, ":test",
			dbus_path, -1);
	t->acquire = test_transport_acquire;
	return t;
}

CK_START_TEST(test_a2dp_sbc) {

	struct ba_transport *t1 = test_transport_new_a2dp(device1,
			BA_TRANSPORT_PROFILE_A2DP_SOURCE, "/path/sbc", &a2dp_sbc_source,
			&config_sbc_44100_stereo);
	struct ba_transport *t2 = test_transport_new_a2dp(device2,
			BA_TRANSPORT_PROFILE_A2DP_SINK, "/path/sbc", &a2dp_sbc_sink,
			&config_sbc_44100_stereo);

	struct ba_transport_pcm *t1_pcm = &t1->media.pcm;
	struct ba_transport_pcm *t2_pcm = &t2->media.pcm;

	if (aging_duration) {
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 153 * 3;
		test_io(t1_pcm, t2_pcm, a2dp_sbc_enc_thread, a2dp_sbc_dec_thread, 4 * 1024);
	}
	else {
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 153 * 3;
		test_io(t1_pcm, t2_pcm, a2dp_sbc_enc_thread, test_io_thread_dump_bt, 2 * 1024);
		test_io(t1_pcm, t2_pcm, test_io_thread_dump_pcm, a2dp_sbc_dec_thread, 2 * 1024);
	}

	ba_transport_destroy(t1);
	ba_transport_destroy(t2);

} CK_END_TEST

CK_START_TEST(test_a2dp_sbc_invalid_config) {

	const a2dp_sbc_t config_sbc_invalid = {
		.sampling_freq = SBC_SAMPLING_FREQ_44100,
		.channel_mode = SBC_CHANNEL_MODE_STEREO };
	struct ba_transport *t = test_transport_new_a2dp(device1,
			BA_TRANSPORT_PROFILE_A2DP_SOURCE, "/path/sbc", &a2dp_sbc_source,
			&config_sbc_invalid);

	int bt_fds[2];
	ck_assert_int_eq(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, bt_fds), 0);
	debug("Created BT socket pair: %d, %d", bt_fds[0], bt_fds[1]);
	t->mtu_read = t->mtu_write = 153 * 3;
	t->bt_fd = bt_fds[1];

	ck_assert_int_eq(ba_transport_pcm_start(&t->media.pcm, a2dp_sbc_enc_thread, "sbc"), 0);
	ck_assert_int_eq(ba_transport_pcm_state_wait_running(&t->media.pcm), -1);

	ba_transport_destroy(t);
	close(bt_fds[0]);

} CK_END_TEST

static void setup_a2dp_link(struct ba_transport *t_source, struct ba_transport *t_sink,
		size_t mtu, int *fd_pcm_sink, int *fd_pcm_source) {

	int bt_fds[2];
	/* Link transports together using BT socket pair. */
	ck_assert_int_eq(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, bt_fds), 0);
	debug("Created BT socket pair: %d, %d", bt_fds[0], bt_fds[1]);
	t_source->mtu_read = t_source->mtu_write = mtu;
	t_sink->mtu_read = t_sink->mtu_write = mtu;
	t_source->bt_fd = bt_fds[1];
	t_sink->bt_fd = bt_fds[0];

	int pcm_snk_fds[2];
	/* Attach sink PCM to the source transport. */
	ck_assert_int_eq(pipe2(pcm_snk_fds, O_NONBLOCK), 0);
	debug("Created PCM pipe pair: %d, %d", pcm_snk_fds[0], pcm_snk_fds[1]);
	t_source->media.pcm.fd = pcm_snk_fds[0];
	t_source->media.pcm.paused = false;

	int pcm_src_fds[2];
	/* Attach source PCM to the sink transport. */
	ck_assert_int_eq(pipe2(pcm_src_fds, O_NONBLOCK), 0);
	debug("Created PCM pipe pair: %d, %d", pcm_src_fds[0], pcm_src_fds[1]);
	t_sink->media.pcm.fd = pcm_src_fds[1];
	t_sink->media.pcm.paused = false;

	*fd_pcm_sink = pcm_snk_fds[1];
	*fd_pcm_source = pcm_src_fds[0];

}

CK_START_TEST(test_a2dp_sbc_pcm_drain) {

	int16_t pcm_zero[90] = { 0 };
	unsigned int nread = 0;

	struct ba_transport *t1 = test_transport_new_a2dp(device1,
			BA_TRANSPORT_PROFILE_A2DP_SOURCE, "/path/sbc", &a2dp_sbc_source,
			&config_sbc_44100_stereo);
	struct ba_transport *t2 = test_transport_new_a2dp(device2,
			BA_TRANSPORT_PROFILE_A2DP_SINK, "/path/sbc", &a2dp_sbc_sink,
			&config_sbc_44100_stereo);

	int fd_pcm_snk = -1;
	int fd_pcm_src = -1;
	setup_a2dp_link(t1, t2, 256, &fd_pcm_snk, &fd_pcm_src);

	/* start sink PCM IO thread and make sure it is running */
	struct ba_transport_pcm *pcm = &t1->media.pcm;
	ck_assert_int_eq(ba_transport_pcm_start(pcm, a2dp_sbc_enc_thread, "sbc"), 0);
	ck_assert_int_eq(ba_transport_pcm_state_wait_running(pcm), 0);

	/* write zero samples to sink PCM until it is full */
	while (write(fd_pcm_snk, pcm_zero, sizeof(pcm_zero)) > 0)
		continue;

	/* verify that the FIFO is not empty */
	ck_assert_int_eq(ioctl(fd_pcm_snk, FIONREAD, &nread), 0);
	ck_assert_uint_gt(nread, 0);

	/* drain PCM samples */
	ck_assert_int_eq(ba_transport_pcm_drain(pcm), 0);

	/* verify that the FIFO has been drained */
	ck_assert_int_eq(ioctl(fd_pcm_snk, FIONREAD, &nread), 0);
	ck_assert_uint_eq(nread, 0);

	ba_transport_destroy(t1);
	ba_transport_destroy(t2);
	close(fd_pcm_snk);
	close(fd_pcm_src);

} CK_END_TEST

static void *test_transport_destroy(void *userdata) {
	struct ba_transport *t = userdata;
	usleep(10000);
	ba_transport_destroy(t);
	return NULL;
}

CK_START_TEST(test_a2dp_sbc_pcm_drain_and_close) {

	int16_t pcm_zero[90] = { 0 };

	struct ba_transport *t1 = test_transport_new_a2dp(device1,
			BA_TRANSPORT_PROFILE_A2DP_SOURCE, "/path/sbc", &a2dp_sbc_source,
			&config_sbc_44100_stereo);
	struct ba_transport *t2 = test_transport_new_a2dp(device2,
			BA_TRANSPORT_PROFILE_A2DP_SINK, "/path/sbc", &a2dp_sbc_sink,
			&config_sbc_44100_stereo);

	int fd_pcm_snk = -1;
	int fd_pcm_src = -1;
	setup_a2dp_link(t1, t2, 256, &fd_pcm_snk, &fd_pcm_src);

	/* start sink PCM IO thread and make sure it is running */
	struct ba_transport_pcm *pcm = &t1->media.pcm;
	ck_assert_int_eq(ba_transport_pcm_start(pcm, a2dp_sbc_enc_thread, "sbc"), 0);
	ck_assert_int_eq(ba_transport_pcm_state_wait_running(pcm), 0);

	/* write zero samples to sink PCM until it is full */
	while (write(fd_pcm_snk, pcm_zero, sizeof(pcm_zero)) > 0)
		continue;

	pthread_t thread;
	/* schedule transport destruction to kick in during PCM drain */
	pthread_create(&thread, NULL, test_transport_destroy, ba_transport_ref(t1));

	/* drain PCM samples */
	ck_assert_int_eq(ba_transport_pcm_drain(pcm), 0);

	pthread_join(thread, NULL);

	ba_transport_destroy(t1);
	ba_transport_destroy(t2);
	close(fd_pcm_snk);
	close(fd_pcm_src);

} CK_END_TEST

CK_START_TEST(test_a2dp_sbc_pcm_drop) {

	int16_t pcm_zero[90] = { 0 };
	int16_t pcm_rand[ARRAYSIZE(pcm_zero)];
	for (size_t i = 0; i < ARRAYSIZE(pcm_rand); i++)
		pcm_rand[i] = rand();

	struct ba_transport *t1 = test_transport_new_a2dp(device1,
			BA_TRANSPORT_PROFILE_A2DP_SOURCE, "/path/sbc", &a2dp_sbc_source,
			&config_sbc_44100_stereo);
	struct ba_transport *t2 = test_transport_new_a2dp(device2,
			BA_TRANSPORT_PROFILE_A2DP_SINK, "/path/sbc", &a2dp_sbc_sink,
			&config_sbc_44100_stereo);

	int fd_pcm_snk = -1;
	int fd_pcm_src = -1;
	setup_a2dp_link(t1, t2, 256, &fd_pcm_snk, &fd_pcm_src);

	/* write zero samples to sink PCM until it is full */
	while (write(fd_pcm_snk, pcm_zero, sizeof(pcm_zero)) > 0)
		continue;

	/* drop sink PCM samples before IO thread was started */
	struct ba_transport_pcm *pcm = &t1->media.pcm;
	ck_assert_int_eq(ba_transport_pcm_drop(pcm), 0);

	/* start IO thread and make sure it is running */
	ck_assert_int_eq(ba_transport_pcm_start(pcm, a2dp_sbc_enc_thread, "sbc"), 0);
	ck_assert_int_eq(ba_transport_pcm_state_wait_running(pcm), 0);

	/* wait for 50 ms - let the thread to run for a while */
	usleep(50000);

	uint8_t bt_buffer[1024];
	/* try to read data from BT socket - it should be empty because
	 * the PCM has been dropped before the thread was started */
	ck_assert_int_eq(read(t2->bt_fd, bt_buffer, sizeof(bt_buffer)), -1);
	ck_assert_int_eq(errno, EAGAIN);

	/* write non-zero samples to sink PCM and process some of it */
	while (write(fd_pcm_snk, pcm_rand, sizeof(pcm_rand)) > 0)
		continue;
	usleep(50000);

	/* drop PCM samples */
	ck_assert_int_eq(ba_transport_pcm_drop(pcm), 0);

	/* write a little bit of non-zero samples and wait for processing; the
	 * number of samples shall be small enough to not produce a single codec
	 * frame, but enough to fill-in internal buffers */
	ck_assert_int_gt(write(fd_pcm_snk, pcm_rand, sizeof(pcm_rand)), 0);
	usleep(10000);

	/* again drop PCM samples */
	ck_assert_int_eq(ba_transport_pcm_drop(pcm), 0);

	/* flush already processed data */
	while (read(t2->bt_fd, bt_buffer, sizeof(bt_buffer)) > 0)
		continue;

	/* After PCM has been dropped, IO thread should not process any more
	 * non-zero samples. We will check this by writing zero samples and
	 * checking if decoded data is all silence. */

	ck_assert_int_eq(ba_transport_pcm_start(&t2->media.pcm, a2dp_sbc_dec_thread, "sbc"), 0);
	ck_assert_int_eq(ba_transport_pcm_state_wait_running(&t2->media.pcm), 0);

	/* write some zero samples to sink PCM and process them */
	for (size_t i = 0; i < 100; i++)
		if (write(fd_pcm_snk, pcm_zero, sizeof(pcm_zero)) <= 0)
			break;
	usleep(250000);

	ssize_t rv;
	int16_t pcm_buffer[1024];
	/* read decoded data and check if it is all silence */
	while ((rv = read(fd_pcm_src, pcm_buffer, sizeof(pcm_buffer))) > 0) {
		const size_t samples = rv / sizeof(pcm_buffer[0]);
		for (size_t i = 0; i < samples; i++)
			ck_assert_int_eq(pcm_buffer[i], 0);
	}

	ba_transport_destroy(t1);
	ba_transport_destroy(t2);
	close(fd_pcm_snk);
	close(fd_pcm_src);

} CK_END_TEST

#if ENABLE_MP3LAME
CK_START_TEST(test_a2dp_mp3) {

	config_mp3_44100_stereo.vbr = enable_vbr_mode ? 1 : 0;

	struct ba_transport *t1 = test_transport_new_a2dp(device1,
			BA_TRANSPORT_PROFILE_A2DP_SOURCE, "/path/mp3", &a2dp_mpeg_source,
			&config_mp3_44100_stereo);
	struct ba_transport *t2 = test_transport_new_a2dp(device2,
			BA_TRANSPORT_PROFILE_A2DP_SINK, "/path/mp3", &a2dp_mpeg_sink,
			&config_mp3_44100_stereo);

	struct ba_transport_pcm *t1_pcm = &t1->media.pcm;
	struct ba_transport_pcm *t2_pcm = &t2->media.pcm;

	if (aging_duration) {
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 1024;
		test_io(t1_pcm, t2_pcm, a2dp_mp3_enc_thread, a2dp_mpeg_dec_thread, 4 * 1024);
	}
	else {
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 512;
		test_io(t1_pcm, t2_pcm, a2dp_mp3_enc_thread, test_io_thread_dump_bt, 3 * 1024);
		test_io(t1_pcm, t2_pcm, test_io_thread_dump_pcm, a2dp_mpeg_dec_thread, 3 * 1024);
	}

	ba_transport_destroy(t1);
	ba_transport_destroy(t2);

} CK_END_TEST
#endif

#if ENABLE_AAC
CK_START_TEST(test_a2dp_aac) {

	config.aac_afterburner = true;
	config.aac_prefer_vbr = enable_vbr_mode;
	config_aac_44100_stereo.vbr = enable_vbr_mode ? 1 : 0;

	struct ba_transport *t1 = test_transport_new_a2dp(device1,
			BA_TRANSPORT_PROFILE_A2DP_SOURCE, "/path/aac", &a2dp_aac_source,
			&config_aac_44100_stereo);
	struct ba_transport *t2 = test_transport_new_a2dp(device2,
			BA_TRANSPORT_PROFILE_A2DP_SINK, "/path/aac", &a2dp_aac_sink,
			&config_aac_44100_stereo);

	struct ba_transport_pcm *t1_pcm = &t1->media.pcm;
	struct ba_transport_pcm *t2_pcm = &t2->media.pcm;

	if (aging_duration) {
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 450;
		test_io(t1_pcm, t2_pcm, a2dp_aac_enc_thread, a2dp_aac_dec_thread, 4 * 1024);
	}
	else {
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 190;
		test_io(t1_pcm, t2_pcm, a2dp_aac_enc_thread, test_io_thread_dump_bt, 2 * 1024);
		test_io(t1_pcm, t2_pcm, test_io_thread_dump_pcm, a2dp_aac_dec_thread, 2 * 1024);
	}

	ba_transport_destroy(t1);
	ba_transport_destroy(t2);

} CK_END_TEST
#endif

#if ENABLE_AAC
CK_START_TEST(test_a2dp_aac_configuration_select) {

	static const unsigned int channels[] = { 1, 2, 6, 8 };
	static const unsigned int rates[] = {
		8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 64000, 88200, 96000 };

	/* Probe AAC encoder for supported configurations. */
	a2dp_aac_source.init(&a2dp_aac_source);

	/* Check whether we are able to run the encoder with all
	 * allegedly supported configurations. */

	for (size_t i = 0; i < ARRAYSIZE(channels); i++)
		for (size_t j = 0; j < ARRAYSIZE(rates); j++) {

			a2dp_aac_t config_aac;
			/* Initialize all bit-fields to 1. */
			memset(&config_aac, 0xFF, sizeof(config_aac));

			a2dp_aac_source.caps_helpers->select_channel_mode(&config_aac, A2DP_MAIN, channels[i]);
			a2dp_aac_source.caps_helpers->select_sample_rate(&config_aac, A2DP_MAIN, rates[j]);

			/* Select the configuration based on the pre-selected capabilities. */
			if (a2dp_aac_source.configuration_select(&a2dp_aac_source, &config_aac) != 0) {
				debug("AAC unsupported configuration: channels=%u, rate=%u",
						channels[i], rates[j]);
				continue;
			}

			hexdump("AAC configuration", &config_aac, sizeof(config_aac));
			struct ba_transport *t = test_transport_new_a2dp(device1,
					BA_TRANSPORT_PROFILE_A2DP_SOURCE, "/path/aac", &a2dp_aac_source,
					&config_aac);

			int bt_fds[2];
			ck_assert_int_eq(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, bt_fds), 0);
			debug("Created BT socket pair: %d, %d", bt_fds[0], bt_fds[1]);
			t->mtu_read = t->mtu_write = 153 * 3;
			t->bt_fd = bt_fds[1];

			ck_assert_int_eq(ba_transport_pcm_start(&t->media.pcm, a2dp_aac_enc_thread, "aac"), 0);
			ck_assert_int_eq(ba_transport_pcm_state_wait_running(&t->media.pcm), 0);

			ba_transport_destroy(t);
			close(bt_fds[0]);

		}

} CK_END_TEST
#endif

#if ENABLE_APTX_IO_TEST
CK_START_TEST(test_a2dp_aptx) {

	struct ba_transport *t1 = test_transport_new_a2dp(device1,
			BA_TRANSPORT_PROFILE_A2DP_SOURCE, "/path/aptx", &a2dp_aptx_source,
			&config_aptx_44100_stereo);
	struct ba_transport *t2 = test_transport_new_a2dp(device2,
			BA_TRANSPORT_PROFILE_A2DP_SINK, "/path/aptx", &a2dp_aptx_sink,
			&config_aptx_44100_stereo);

	struct ba_transport_pcm *t1_pcm = &t1->media.pcm;
	struct ba_transport_pcm *t2_pcm = &t2->media.pcm;

	if (aging_duration) {
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 400;
		test_io(t1_pcm, t2_pcm, a2dp_aptx_enc_thread, a2dp_aptx_dec_thread, 4 * 1024);
	}
	else {
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 40;
		test_io(t1_pcm, t2_pcm, a2dp_aptx_enc_thread, test_io_thread_dump_bt, 2 * 1024);
		test_io(t1_pcm, t2_pcm, test_io_thread_dump_pcm, a2dp_aptx_dec_thread, 2 * 1024);
	};

	ba_transport_destroy(t1);
	ba_transport_destroy(t2);

} CK_END_TEST
#endif

#if ENABLE_APTX_HD_IO_TEST
CK_START_TEST(test_a2dp_aptx_hd) {

	struct ba_transport *t1 = test_transport_new_a2dp(device1,
			BA_TRANSPORT_PROFILE_A2DP_SOURCE, "/path/aptxhd", &a2dp_aptx_hd_source,
			&config_aptx_hd_44100_stereo);
	struct ba_transport *t2 = test_transport_new_a2dp(device2,
			BA_TRANSPORT_PROFILE_A2DP_SINK, "/path/aptxhd", &a2dp_aptx_hd_sink,
			&config_aptx_hd_44100_stereo);

	struct ba_transport_pcm *t1_pcm = &t1->media.pcm;
	struct ba_transport_pcm *t2_pcm = &t2->media.pcm;

	if (aging_duration) {
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 600;
		test_io(t1_pcm, t2_pcm, a2dp_aptx_hd_enc_thread, a2dp_aptx_hd_dec_thread, 4 * 1024);
	}
	else {
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 60;
		test_io(t1_pcm, t2_pcm, a2dp_aptx_hd_enc_thread, test_io_thread_dump_bt, 2 * 1024);
		test_io(t1_pcm, t2_pcm, test_io_thread_dump_pcm, a2dp_aptx_hd_dec_thread, 2 * 1024);
	};

	ba_transport_destroy(t1);
	ba_transport_destroy(t2);

} CK_END_TEST
#endif

#if ENABLE_FASTSTREAM
CK_START_TEST(test_a2dp_faststream_music) {

	struct ba_transport *t1 = test_transport_new_a2dp(device1,
			BA_TRANSPORT_PROFILE_A2DP_SOURCE, "/path/faststream", &a2dp_faststream_source,
			&config_faststream_44100_16000);
	struct ba_transport *t2 = test_transport_new_a2dp(device2,
			BA_TRANSPORT_PROFILE_A2DP_SINK, "/path/faststream", &a2dp_faststream_sink,
			&config_faststream_44100_16000);

	struct ba_transport_pcm *t1_pcm = &t1->media.pcm;
	struct ba_transport_pcm *t2_pcm = &t2->media.pcm;

	if (aging_duration) {
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 72 * 3;
		test_io(t1_pcm, t2_pcm, a2dp_fs_enc_thread, a2dp_fs_dec_thread, 4 * 1024);
	}
	else {
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 72 * 3;
		test_io(t1_pcm, t2_pcm, a2dp_fs_enc_thread, test_io_thread_dump_bt, 2 * 1024);
		test_io(t1_pcm, t2_pcm, test_io_thread_dump_pcm, a2dp_fs_dec_thread, 2 * 1024);
	}

	ba_transport_destroy(t1);
	ba_transport_destroy(t2);

} CK_END_TEST
#endif

#if ENABLE_FASTSTREAM
CK_START_TEST(test_a2dp_faststream_voice) {

	struct ba_transport *t1 = test_transport_new_a2dp(device1,
			BA_TRANSPORT_PROFILE_A2DP_SOURCE, "/path/faststream", &a2dp_faststream_source,
			&config_faststream_44100_16000);
	struct ba_transport *t2 = test_transport_new_a2dp(device2,
			BA_TRANSPORT_PROFILE_A2DP_SINK, "/path/faststream", &a2dp_faststream_sink,
			&config_faststream_44100_16000);

	struct ba_transport_pcm *t1_pcm_bc = &t1->media.pcm_bc;
	struct ba_transport_pcm *t2_pcm_bc = &t2->media.pcm_bc;

	if (aging_duration) {
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 72 * 3;
		test_io(t2_pcm_bc, t1_pcm_bc, a2dp_fs_enc_thread, a2dp_fs_dec_thread, 4 * 1024);
	}
	else {
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 72 * 3;
		test_io(t2_pcm_bc, t1_pcm_bc, a2dp_fs_enc_thread, test_io_thread_dump_bt, 2 * 1024);
		test_io(t2_pcm_bc, t1_pcm_bc, test_io_thread_dump_pcm, a2dp_fs_dec_thread, 2 * 1024);
	}

	ba_transport_destroy(t1);
	ba_transport_destroy(t2);

} CK_END_TEST
#endif

#if ENABLE_LC3PLUS
CK_START_TEST(test_a2dp_lc3plus) {

	struct ba_transport *t1 = test_transport_new_a2dp(device1,
			BA_TRANSPORT_PROFILE_A2DP_SOURCE, "/path/lc3plus", &a2dp_lc3plus_source,
			&config_lc3plus_48000_stereo);
	struct ba_transport *t2 = test_transport_new_a2dp(device2,
			BA_TRANSPORT_PROFILE_A2DP_SINK, "/path/lc3plus", &a2dp_lc3plus_sink,
			&config_lc3plus_48000_stereo);

	struct ba_transport_pcm *t1_pcm = &t1->media.pcm;
	struct ba_transport_pcm *t2_pcm = &t2->media.pcm;

	if (aging_duration) {
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write =
			RTP_HEADER_LEN + sizeof(rtp_media_header_t) + 300;
		test_io(t1_pcm, t2_pcm, a2dp_lc3plus_enc_thread, a2dp_lc3plus_dec_thread, 4 * 1024);
	}
	else {
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write =
			RTP_HEADER_LEN + sizeof(rtp_media_header_t) + 300;
		test_io(t1_pcm, t2_pcm, a2dp_lc3plus_enc_thread, test_io_thread_dump_bt, 2 * 1024);
		test_io(t1_pcm, t2_pcm, test_io_thread_dump_pcm, a2dp_lc3plus_dec_thread, 2 * 1024);
	}

	ba_transport_destroy(t1);
	ba_transport_destroy(t2);

} CK_END_TEST
#endif

#if ENABLE_LDAC_IO_TEST
CK_START_TEST(test_a2dp_ldac) {

	config.ldac_abr = true;
	config.ldac_eqmid = LDACBT_EQMID_HQ;

	struct ba_transport *t1 = test_transport_new_a2dp(device1,
			BA_TRANSPORT_PROFILE_A2DP_SOURCE, "/path/ldac", &a2dp_ldac_source,
			&config_ldac_48000_stereo);
	struct ba_transport *t2 = test_transport_new_a2dp(device2,
			BA_TRANSPORT_PROFILE_A2DP_SINK, "/path/ldac", &a2dp_ldac_sink,
			&config_ldac_48000_stereo);

	struct ba_transport_pcm *t1_pcm = &t1->media.pcm;
	struct ba_transport_pcm *t2_pcm = &t2->media.pcm;

	if (aging_duration) {
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write =
			RTP_HEADER_LEN + sizeof(rtp_media_header_t) + 990 + 6;
		test_io(t1_pcm, t2_pcm, a2dp_ldac_enc_thread, a2dp_ldac_dec_thread, 4 * 1024);
	}
	else {
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write =
			RTP_HEADER_LEN + sizeof(rtp_media_header_t) + 660 + 6;
		test_io(t1_pcm, t2_pcm, a2dp_ldac_enc_thread, test_io_thread_dump_bt, 2 * 1024);
		test_io(t1_pcm, t2_pcm, test_io_thread_dump_pcm, a2dp_ldac_dec_thread, 2 * 1024);
	}

	ba_transport_destroy(t1);
	ba_transport_destroy(t2);

} CK_END_TEST
#endif

#if ENABLE_LHDC
CK_START_TEST(test_a2dp_lhdc_v3) {

	config.lhdc_eqmid = LHDCBT_QUALITY_HIGH;

	struct ba_transport *t1 = test_transport_new_a2dp(device1,
			BA_TRANSPORT_PROFILE_A2DP_SOURCE, "/path/lhdc", &a2dp_lhdc_v3_source,
			&config_lhdc_v3_48000_stereo);
	struct ba_transport *t2 = test_transport_new_a2dp(device2,
			BA_TRANSPORT_PROFILE_A2DP_SINK, "/path/lhdc", &a2dp_lhdc_v3_sink,
			&config_lhdc_v3_48000_stereo);

	struct ba_transport_pcm *t1_pcm = &t1->media.pcm;
	struct ba_transport_pcm *t2_pcm = &t2->media.pcm;

	if (aging_duration) {
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 960;
		test_io(t1_pcm, t2_pcm, a2dp_lhdc_enc_thread, a2dp_lhdc_dec_thread, 4 * 1024);
	}
	else {
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 960;
		test_io(t1_pcm, t2_pcm, a2dp_lhdc_enc_thread, test_io_thread_dump_bt, 2 * 1024);
		test_io(t1_pcm, t2_pcm, test_io_thread_dump_pcm, a2dp_lhdc_dec_thread, 2 * 1024);
	}

	ba_transport_destroy(t1);
	ba_transport_destroy(t2);

} CK_END_TEST
#endif

#if ENABLE_OPUS
CK_START_TEST(test_a2dp_opus) {

	struct ba_transport *t1 = test_transport_new_a2dp(device1,
			BA_TRANSPORT_PROFILE_A2DP_SOURCE, "/path/opus", &a2dp_opus_source,
			&config_opus_48000_stereo);
	struct ba_transport *t2 = test_transport_new_a2dp(device2,
			BA_TRANSPORT_PROFILE_A2DP_SINK, "/path/opus", &a2dp_opus_source,
			&config_opus_48000_stereo);

	struct ba_transport_pcm *t1_pcm = &t1->media.pcm;
	struct ba_transport_pcm *t2_pcm = &t2->media.pcm;

	if (aging_duration) {
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 600;
		test_io(t1_pcm, t2_pcm, a2dp_opus_enc_thread, a2dp_opus_dec_thread, 4 * 1024);
	}
	else {
		t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 600;
		test_io(t1_pcm, t2_pcm, a2dp_opus_enc_thread, test_io_thread_dump_bt, 2 * 1024);
		test_io(t1_pcm, t2_pcm, test_io_thread_dump_pcm, a2dp_opus_dec_thread, 2 * 1024);
	}

	ba_transport_destroy(t1);
	ba_transport_destroy(t2);

} CK_END_TEST
#endif

CK_START_TEST(test_sco_cvsd) {

	struct ba_transport *t1 = test_transport_new_sco(device1,
			BA_TRANSPORT_PROFILE_HSP_AG, "/path/sco/cvsd");
	struct ba_transport *t2 = test_transport_new_sco(device2,
			BA_TRANSPORT_PROFILE_HSP_AG, "/path/sco/cvsd");

	struct ba_transport_pcm *t1_pcm = &t1->sco.pcm_spk;
	struct ba_transport_pcm *t2_pcm = &t2->sco.pcm_spk;

	t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 48;
	test_io(t1_pcm, t2_pcm, sco_enc_thread, test_io_thread_dump_bt, 600);
	test_io(t1_pcm, t2_pcm, test_io_thread_dump_pcm, sco_dec_thread, 600);

	ba_transport_destroy(t1);
	ba_transport_destroy(t2);

} CK_END_TEST

#if ENABLE_MSBC
CK_START_TEST(test_sco_msbc) {

	adapter->hci.features[2] = LMP_TRSP_SCO;
	adapter->hci.features[3] = LMP_ESCO;
	config.io_thread_rt_priority = 10;

	struct ba_transport *t1 = test_transport_new_sco(device1,
			BA_TRANSPORT_PROFILE_HFP_AG, "/path/sco/msbc");
	ba_transport_set_codec(t1, HFP_CODEC_MSBC);
	struct ba_transport *t2 = test_transport_new_sco(device2,
			BA_TRANSPORT_PROFILE_HFP_AG, "/path/sco/msbc");
	ba_transport_set_codec(t2, HFP_CODEC_MSBC);

	struct ba_transport_pcm *t1_pcm = &t1->sco.pcm_spk;
	struct ba_transport_pcm *t2_pcm = &t2->sco.pcm_spk;

	t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 24;
	test_io(t1_pcm, t2_pcm, sco_enc_thread, test_io_thread_dump_bt, 600);
	test_io(t1_pcm, t2_pcm, test_io_thread_dump_pcm, sco_dec_thread, 600);

	ba_transport_destroy(t1);
	ba_transport_destroy(t2);

} CK_END_TEST
#endif

#if ENABLE_LC3_SWB
CK_START_TEST(test_sco_lc3_swb) {

	adapter->hci.features[2] = LMP_TRSP_SCO;
	adapter->hci.features[3] = LMP_ESCO;

	struct ba_transport *t1 = test_transport_new_sco(device1,
			BA_TRANSPORT_PROFILE_HFP_AG, "/path/sco/lc3swb");
	ba_transport_set_codec(t1, HFP_CODEC_LC3_SWB);
	struct ba_transport *t2 = test_transport_new_sco(device2,
			BA_TRANSPORT_PROFILE_HFP_AG, "/path/sco/lc3swb");
	ba_transport_set_codec(t2, HFP_CODEC_LC3_SWB);

	struct ba_transport_pcm *t1_pcm = &t1->sco.pcm_spk;
	struct ba_transport_pcm *t2_pcm = &t2->sco.pcm_spk;

	t1->mtu_read = t1->mtu_write = t2->mtu_read = t2->mtu_write = 24;
	test_io(t1_pcm, t2_pcm, sco_enc_thread, test_io_thread_dump_bt, 600);
	test_io(t1_pcm, t2_pcm, test_io_thread_dump_pcm, sco_dec_thread, 600);

	ba_transport_destroy(t1);
	ba_transport_destroy(t2);

} CK_END_TEST
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
		{ a2dp_codecs_codec_id_to_string(A2DP_CODEC_SBC), test_a2dp_sbc_invalid_config },
		{ a2dp_codecs_codec_id_to_string(A2DP_CODEC_SBC), test_a2dp_sbc_pcm_drain },
		{ a2dp_codecs_codec_id_to_string(A2DP_CODEC_SBC), test_a2dp_sbc_pcm_drain_and_close },
		{ a2dp_codecs_codec_id_to_string(A2DP_CODEC_SBC), test_a2dp_sbc_pcm_drop },
#if ENABLE_MP3LAME
		{ a2dp_codecs_codec_id_to_string(A2DP_CODEC_MPEG12), test_a2dp_mp3 },
#endif
#if ENABLE_AAC
		{ a2dp_codecs_codec_id_to_string(A2DP_CODEC_MPEG24), test_a2dp_aac },
		{ a2dp_codecs_codec_id_to_string(A2DP_CODEC_MPEG24), test_a2dp_aac_configuration_select },
#endif
#if ENABLE_APTX_IO_TEST
		{ a2dp_codecs_codec_id_to_string(A2DP_CODEC_VENDOR_ID(APTX_VENDOR_ID, APTX_CODEC_ID)), test_a2dp_aptx },
#endif
#if ENABLE_APTX_HD_IO_TEST
		{ a2dp_codecs_codec_id_to_string(A2DP_CODEC_VENDOR_ID(APTX_HD_VENDOR_ID, APTX_HD_CODEC_ID)), test_a2dp_aptx_hd },
#endif
#if ENABLE_FASTSTREAM
		{ a2dp_codecs_codec_id_to_string(A2DP_CODEC_VENDOR_ID(FASTSTREAM_VENDOR_ID, FASTSTREAM_CODEC_ID)), test_a2dp_faststream_music },
		{ a2dp_codecs_codec_id_to_string(A2DP_CODEC_VENDOR_ID(FASTSTREAM_VENDOR_ID, FASTSTREAM_CODEC_ID)), test_a2dp_faststream_voice },
#endif
#if ENABLE_LC3PLUS
		{ a2dp_codecs_codec_id_to_string(A2DP_CODEC_VENDOR_ID(LC3PLUS_VENDOR_ID, LC3PLUS_CODEC_ID)), test_a2dp_lc3plus },
#endif
#if ENABLE_LDAC_IO_TEST
		{ a2dp_codecs_codec_id_to_string(A2DP_CODEC_VENDOR_ID(LDAC_VENDOR_ID, LDAC_CODEC_ID)), test_a2dp_ldac },
#endif
#if ENABLE_LHDC
		{ a2dp_codecs_codec_id_to_string(A2DP_CODEC_VENDOR_ID(LHDC_V3_VENDOR_ID, LHDC_V3_CODEC_ID)), test_a2dp_lhdc_v3 },
#endif
#if ENABLE_OPUS
		{ a2dp_codecs_codec_id_to_string(A2DP_CODEC_VENDOR_ID(OPUS_VENDOR_ID, OPUS_CODEC_ID)), test_a2dp_opus },
#endif
		{ hfp_codec_id_to_string(HFP_CODEC_CVSD), test_sco_cvsd },
#if ENABLE_MSBC
		{ hfp_codec_id_to_string(HFP_CODEC_MSBC), test_sco_msbc },
#endif
#if ENABLE_LC3_SWB
		{ hfp_codec_id_to_string(HFP_CODEC_LC3_SWB), test_sco_lc3_swb },
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

	unsigned int enabled_codecs = 0xFFFFFFFF;

	if (optind != argc)
		enabled_codecs = 0;

	for (; optind < argc; optind++)
		for (size_t i = 0; i < ARRAYSIZE(codecs); i++)
			if (strcasecmp(argv[optind], codecs[i].name) == 0)
				enabled_codecs |= 1 << i;

	if (input_bt_file != NULL) {

		if ((btdin = bt_dump_open(input_bt_file)) == NULL) {
			error("Couldn't open input BT dump file: %s", strerror(errno));
			return EXIT_FAILURE;
		}

		const char *codec = "";
		switch (btdin->mode) {
		case BT_DUMP_MODE_A2DP_SOURCE:
		case BT_DUMP_MODE_A2DP_SINK:
			codec = a2dp_codecs_codec_id_to_string(btdin->transport_codec_id);
			debug("BT dump A2DP codec: %s (%#x)", codec, btdin->transport_codec_id);
			hexdump("BT dump A2DP configuration",
					&btdin->a2dp_configuration, btdin->a2dp_configuration_size);
			break;
		case BT_DUMP_MODE_SCO:
			codec = hfp_codec_id_to_string(btdin->transport_codec_id);
			debug("BT dump HFP codec: %s (%#x)", codec, btdin->transport_codec_id);
			break;
		}

		enabled_codecs = 0;
		for (size_t i = 0; i < ARRAYSIZE(codecs); i++)
			if (codec != NULL && strcmp(codec, codecs[i].name) == 0)
				enabled_codecs |= 1 << i;

		/* If we do not have a test case for the codec, dump the data here. */
		if (enabled_codecs == 0) {
			ssize_t len;
			char buffer[4096];
			while ((len = bt_dump_read(btdin, buffer, sizeof(buffer))) != -1)
				hexdump("BT data", buffer, len);
		}

	}

	ba_config_init();

	bdaddr_t addr1 = {{ 1, 2, 3, 4, 5, 6 }};
	bdaddr_t addr2 = {{ 1, 2, 3, 7, 8, 9 }};
	adapter = ba_adapter_new(0);
	device1 = ba_device_new(adapter, &addr1);
	device2 = ba_device_new(adapter, &addr2);

	Suite *s = suite_create(__FILE__);
	TCase *tc = tcase_create(__FILE__);
	SRunner *sr = srunner_create(s);

	suite_add_tcase(s, tc);

	tcase_set_timeout(tc, aging_duration + 10);
	if (input_bt_file != NULL || input_pcm_file != NULL)
		tcase_set_timeout(tc, aging_duration + 3600);

	for (size_t i = 0; i < ARRAYSIZE(codecs); i++)
		if (enabled_codecs & (1 << i))
			tcase_add_test(tc, codecs[i].tf);

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);

	srunner_free(sr);
	ba_device_unref(device1);
	ba_device_unref(device2);
	ba_adapter_unref(adapter);
	bt_dump_close(btdin);

	return nf == 0 ? 0 : 1;
}
