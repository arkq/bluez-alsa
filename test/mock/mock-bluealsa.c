/*
 * mock-bluealsa.c
 * Copyright (c) 2016-2023 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "mock.h"
/* IWYU pragma: no_include "config.h" */

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>

#include <gio/gio.h>
#include <glib.h>

#include "a2dp.h"
#if ENABLE_APTX
# include "a2dp-aptx.h"
#endif
#if ENABLE_APTX_HD
# include "a2dp-aptx-hd.h"
#endif
#if ENABLE_FASTSTREAM
# include "a2dp-faststream.h"
#endif
#include "a2dp-sbc.h"
#include "ba-adapter.h"
#include "ba-device.h"
#include "ba-rfcomm.h"
#include "ba-transport.h"
#include "ba-transport-pcm.h"
#include "bluealsa-config.h"
#include "bluez.h"
#include "codec-sbc.h"
#include "hfp.h"
#include "io.h"
#include "shared/a2dp-codecs.h"
#include "shared/defs.h"
#include "shared/log.h"
#include "shared/rt.h"

#include "inc/sine.inc"

#define TEST_BLUEALSA_STORAGE_DIR "/tmp/bluealsa-mock-storage"

static const a2dp_sbc_t config_sbc_44100_stereo = {
	.frequency = SBC_SAMPLING_FREQ_44100,
	.channel_mode = SBC_CHANNEL_MODE_JOINT_STEREO,
	.block_length = SBC_BLOCK_LENGTH_16,
	.subbands = SBC_SUBBANDS_8,
	.allocation_method = SBC_ALLOCATION_LOUDNESS,
	.min_bitpool = SBC_MIN_BITPOOL,
	.max_bitpool = SBC_MAX_BITPOOL,
};

#if ENABLE_APTX
static const a2dp_aptx_t config_aptx_44100_stereo = {
	.info = A2DP_SET_VENDOR_ID_CODEC_ID(APTX_VENDOR_ID, APTX_CODEC_ID),
	.channel_mode = APTX_CHANNEL_MODE_STEREO,
	.frequency = APTX_SAMPLING_FREQ_44100,
};
#endif

#if ENABLE_APTX_HD
static const a2dp_aptx_hd_t config_aptx_hd_48000_stereo = {
	.aptx.info = A2DP_SET_VENDOR_ID_CODEC_ID(APTX_HD_VENDOR_ID, APTX_HD_CODEC_ID),
	.aptx.channel_mode = APTX_CHANNEL_MODE_STEREO,
	.aptx.frequency = APTX_SAMPLING_FREQ_48000,
};
#endif

#if ENABLE_FASTSTREAM
static const a2dp_faststream_t config_faststream_44100_16000 = {
	.info = A2DP_SET_VENDOR_ID_CODEC_ID(FASTSTREAM_VENDOR_ID, FASTSTREAM_CODEC_ID),
	.direction = FASTSTREAM_DIRECTION_MUSIC | FASTSTREAM_DIRECTION_VOICE,
	.frequency_music = FASTSTREAM_SAMPLING_FREQ_MUSIC_44100,
	.frequency_voice = FASTSTREAM_SAMPLING_FREQ_VOICE_16000,
};
#endif

bool bluez_a2dp_set_configuration(const char *current_dbus_sep_path,
		const struct a2dp_sep *sep, GError **error) {
	debug("%s: %s", __func__, current_dbus_sep_path);
	(void)current_dbus_sep_path; (void)sep;
	*error = g_error_new(G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED, "Not supported");
	return false;
}

void bluez_battery_provider_update(struct ba_device *device) {
	debug("%s: %p", __func__, device);
	(void)device;
}

int ofono_call_volume_update(struct ba_transport *transport) {
	debug("%s: %p", __func__, transport);
	(void)transport;
	return 0;
}

static void *mock_dec(struct ba_transport_pcm *t_pcm) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pcm_thread_cleanup), t_pcm);

	const unsigned int channels = t_pcm->channels;
	const unsigned int samplerate = t_pcm->sampling;
	struct pollfd fds[1] = {{ t_pcm->pipe[0], POLLIN, 0 }};
	struct asrsync asrs = { .frames = 0 };
	int16_t buffer[1024 * 2];
	int x = 0;

	debug_transport_pcm_thread_loop(t_pcm, "START");
	for (ba_transport_pcm_state_set_running(t_pcm);;) {

		int timeout = 0;
		if (!ba_transport_pcm_is_active(t_pcm))
			timeout = -1;

		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
		int poll_rv = poll(fds, ARRAYSIZE(fds), timeout);
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

		if (poll_rv == 1 && fds[0].revents & POLLIN) {
			/* dispatch incoming event */
			switch (ba_transport_pcm_signal_recv(t_pcm)) {
			case BA_TRANSPORT_PCM_SIGNAL_OPEN:
			case BA_TRANSPORT_PCM_SIGNAL_RESUME:
				asrs.frames = 0;
				continue;
			default:
				continue;
			}
		}

		fprintf(stderr, ".");

		if (asrs.frames == 0)
			asrsync_init(&asrs, samplerate);

		const size_t samples = ARRAYSIZE(buffer);
		const size_t frames = samples / channels;
		x = snd_pcm_sine_s16_2le(buffer, frames, channels, x, 146.83 / samplerate);

		io_pcm_scale(t_pcm, buffer, samples);
		if (io_pcm_write(t_pcm, buffer, samples) == -1)
			error("FIFO write error: %s", strerror(errno));

		/* maintain constant speed */
		asrsync_sync(&asrs, frames);

	}

	pthread_cleanup_pop(1);
	return NULL;
}

void *a2dp_sbc_dec_thread(struct ba_transport_pcm *t_pcm) { return mock_dec(t_pcm); }
void *a2dp_mpeg_dec_thread(struct ba_transport_pcm *t_pcm) { return mock_dec(t_pcm); }
void *a2dp_aac_dec_thread(struct ba_transport_pcm *t_pcm) { return mock_dec(t_pcm); }
void *a2dp_aptx_dec_thread(struct ba_transport_pcm *t_pcm) { return mock_dec(t_pcm); }
void *a2dp_aptx_hd_dec_thread(struct ba_transport_pcm *t_pcm) { return mock_dec(t_pcm); }
void *a2dp_faststream_dec_thread(struct ba_transport_pcm *t_pcm) { return mock_dec(t_pcm); }
void *sco_dec_thread(struct ba_transport_pcm *t_pcm) { return mock_dec(t_pcm); }

static void *mock_bt_dump_thread(void *userdata) {

	int bt_fd = GPOINTER_TO_INT(userdata);
	FILE *f_output = NULL;
	uint8_t buffer[1024];
	ssize_t len;

	if (mock_dump_output)
		f_output = fopen("bluealsa-mock.dump", "w");

	debug("IO loop: START: %s", __func__);
	while ((len = read(bt_fd, buffer, sizeof(buffer))) > 0) {
		fprintf(stderr, "#");

		if (!mock_dump_output)
			continue;

		for (ssize_t i = 0; i < len; i++)
			fprintf(f_output, "%02x", buffer[i]);
		fprintf(f_output, "\n");

	}

	debug("IO loop: EXIT: %s", __func__);
	if (f_output != NULL)
		fclose(f_output);
	close(bt_fd);
	return NULL;
}

static int mock_transport_set_a2dp_state_active(struct ba_transport *t) {
	ba_transport_set_a2dp_state(t, BLUEZ_A2DP_TRANSPORT_STATE_ACTIVE);
	return G_SOURCE_REMOVE;
}

static int mock_transport_acquire_bt(struct ba_transport *t) {

	int bt_fds[2];
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, bt_fds) == 0);

	t->bt_fd = bt_fds[0];
	t->mtu_read = 256;
	t->mtu_write = 256;

	if (t->profile & BA_TRANSPORT_PROFILE_MASK_SCO)
		t->mtu_read = t->mtu_write = 48;

	debug("New transport: %d (MTU: R:%zu W:%zu)", t->bt_fd, t->mtu_read, t->mtu_write);

	g_thread_unref(g_thread_new(NULL, mock_bt_dump_thread, GINT_TO_POINTER(bt_fds[1])));

	if (t->profile & BA_TRANSPORT_PROFILE_MASK_A2DP)
		/* Emulate asynchronous transport activation by BlueZ. */
		g_timeout_add(10, G_SOURCE_FUNC(mock_transport_set_a2dp_state_active), t);

	return bt_fds[0];
}

static struct ba_device *mock_device_new(struct ba_adapter *a, const char *btmac) {

	bdaddr_t addr;
	str2ba(btmac, &addr);

	struct ba_device *d;
	if ((d = ba_device_lookup(a, &addr)) == NULL) {
		d = ba_device_new(a, &addr);
		d->battery.charge = 75;
	}

	return d;
}

static struct ba_transport *mock_transport_new_a2dp(const char *device_btmac,
		uint16_t profile, const char *dbus_path, const struct a2dp_codec *codec,
		const void *configuration) {

	usleep(mock_fuzzing_ms * 1000);

	struct ba_device *d = mock_device_new(mock_adapter, device_btmac);
	const char *dbus_owner = g_dbus_connection_get_unique_name(config.dbus);
	struct ba_transport *t = ba_transport_new_a2dp(d, profile, dbus_owner, dbus_path,
			codec, configuration);
	t->acquire = mock_transport_acquire_bt;

	fprintf(stderr, "BLUEALSA_PCM_READY=A2DP:%s:%s\n", device_btmac,
			a2dp_codecs_codec_id_to_string(ba_transport_get_codec(t)));

	ba_transport_set_a2dp_state(t, BLUEZ_A2DP_TRANSPORT_STATE_PENDING);

	ba_device_unref(d);
	return t;
}

static void *mock_transport_rfcomm_thread(void *userdata) {

	static const struct {
		const char *command;
		const char *response;
	} responses[] = {
		/* accept HFP codec selection */
		{ "\r\n+BCS:1\r\n", "AT+BCS=1\r" },
		{ "\r\n+BCS:2\r\n", "AT+BCS=2\r" },
	};

	int rfcomm_fd = GPOINTER_TO_INT(userdata);
	char buffer[1024];
	ssize_t len;

	while ((len = read(rfcomm_fd, buffer, sizeof(buffer))) > 0) {
		hexdump("RFCOMM", buffer, len, true);

		for (size_t i = 0; i < ARRAYSIZE(responses); i++) {
			if (strncmp(buffer, responses[i].command, len) != 0)
				continue;
			len = strlen(responses[i].response);
			if (write(rfcomm_fd, responses[i].response, len) != len)
				warn("Couldn't write RFCOMM response: %s", strerror(errno));
			break;
		}

	}

	close(rfcomm_fd);
	return NULL;
}

static struct ba_transport *mock_transport_new_sco(const char *device_btmac,
		uint16_t profile, const char *dbus_path) {

	usleep(mock_fuzzing_ms * 1000);

	struct ba_device *d = mock_device_new(mock_adapter, device_btmac);
	const char *dbus_owner = g_dbus_connection_get_unique_name(config.dbus);

	int fds[2];
	socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
	g_thread_unref(g_thread_new(NULL, mock_transport_rfcomm_thread, GINT_TO_POINTER(fds[1])));

	struct ba_transport *t = ba_transport_new_sco(d, profile, dbus_owner, dbus_path, fds[0]);
	t->sco.rfcomm->state = HFP_SLC_CONNECTED;
	t->sco.rfcomm->ag_codecs.cvsd = true;
	t->sco.rfcomm->hf_codecs.cvsd = true;
#if ENABLE_MSBC
	t->sco.rfcomm->ag_features |= HFP_AG_FEAT_CODEC | HFP_AG_FEAT_ESCO;
	t->sco.rfcomm->hf_features |= HFP_HF_FEAT_CODEC | HFP_HF_FEAT_ESCO;
	t->sco.rfcomm->ag_codecs.msbc = true;
	t->sco.rfcomm->hf_codecs.msbc = true;
#endif
	t->acquire = mock_transport_acquire_bt;

	fprintf(stderr, "BLUEALSA_PCM_READY=SCO:%s:%s\n", device_btmac,
			hfp_codec_id_to_string(ba_transport_get_codec(t)));

	ba_device_unref(d);
	return t;
}

static void *mock_bluealsa_service_thread(void *userdata) {
	(void)userdata;

	GPtrArray *tt = g_ptr_array_new();
	size_t i;

	if (config.profile.a2dp_source) {

		if (a2dp_sbc_source.enabled)
			g_ptr_array_add(tt, mock_transport_new_a2dp(MOCK_DEVICE_1,
						BA_TRANSPORT_PROFILE_A2DP_SOURCE, MOCK_BLUEZ_MEDIA_TRANSPORT_PATH_1,
						&a2dp_sbc_source, &config_sbc_44100_stereo));

#if ENABLE_APTX
		if (a2dp_aptx_source.enabled)
			g_ptr_array_add(tt, mock_transport_new_a2dp(MOCK_DEVICE_2,
						BA_TRANSPORT_PROFILE_A2DP_SOURCE, MOCK_BLUEZ_MEDIA_TRANSPORT_PATH_2,
						&a2dp_aptx_source, &config_aptx_44100_stereo));
		else
#endif
#if ENABLE_APTX_HD
		if (a2dp_aptx_hd_source.enabled)
			g_ptr_array_add(tt, mock_transport_new_a2dp(MOCK_DEVICE_2,
						BA_TRANSPORT_PROFILE_A2DP_SOURCE, MOCK_BLUEZ_MEDIA_TRANSPORT_PATH_2,
						&a2dp_aptx_hd_source, &config_aptx_hd_48000_stereo));
		else
#endif
#if ENABLE_FASTSTREAM
		if (a2dp_faststream_source.enabled)
			g_ptr_array_add(tt, mock_transport_new_a2dp(MOCK_DEVICE_2,
						BA_TRANSPORT_PROFILE_A2DP_SOURCE, MOCK_BLUEZ_MEDIA_TRANSPORT_PATH_2,
						&a2dp_faststream_source, &config_faststream_44100_16000));
		else
#endif
		if (a2dp_sbc_source.enabled)
			g_ptr_array_add(tt, mock_transport_new_a2dp(MOCK_DEVICE_2,
						BA_TRANSPORT_PROFILE_A2DP_SOURCE, MOCK_BLUEZ_MEDIA_TRANSPORT_PATH_2,
						&a2dp_sbc_source, &config_sbc_44100_stereo));

	}

	if (config.profile.a2dp_sink) {

#if ENABLE_APTX_HD
		if (a2dp_aptx_hd_sink.enabled)
			g_ptr_array_add(tt, mock_transport_new_a2dp(MOCK_DEVICE_1,
						BA_TRANSPORT_PROFILE_A2DP_SINK, MOCK_BLUEZ_MEDIA_TRANSPORT_PATH_1,
						&a2dp_aptx_hd_sink, &config_aptx_hd_48000_stereo));
		else
#endif
#if ENABLE_APTX
		if (a2dp_aptx_sink.enabled)
			g_ptr_array_add(tt, mock_transport_new_a2dp(MOCK_DEVICE_1,
						BA_TRANSPORT_PROFILE_A2DP_SINK, MOCK_BLUEZ_MEDIA_TRANSPORT_PATH_1,
						&a2dp_aptx_sink, &config_aptx_44100_stereo));
		else
#endif
		if (a2dp_sbc_sink.enabled)
			g_ptr_array_add(tt, mock_transport_new_a2dp(MOCK_DEVICE_1,
						BA_TRANSPORT_PROFILE_A2DP_SINK, MOCK_BLUEZ_MEDIA_TRANSPORT_PATH_1,
						&a2dp_sbc_sink, &config_sbc_44100_stereo));

		if (a2dp_sbc_sink.enabled)
			g_ptr_array_add(tt, mock_transport_new_a2dp(MOCK_DEVICE_2,
						BA_TRANSPORT_PROFILE_A2DP_SINK, MOCK_BLUEZ_MEDIA_TRANSPORT_PATH_2,
						&a2dp_sbc_sink, &config_sbc_44100_stereo));

	}

	if (config.profile.hfp_ag) {

		struct ba_transport *t;
		g_ptr_array_add(tt, t = mock_transport_new_sco(MOCK_DEVICE_1,
					BA_TRANSPORT_PROFILE_HFP_AG, MOCK_BLUEZ_SCO_PATH_1));

		if (mock_fuzzing_ms)
			ba_transport_set_codec(t, HFP_CODEC_CVSD);

#if ENABLE_MSBC
		if (mock_fuzzing_ms) {
			usleep(mock_fuzzing_ms * 1000);
			ba_transport_set_codec(t, HFP_CODEC_MSBC);
		}
#endif

	}

	if (config.profile.hsp_ag) {
		g_ptr_array_add(tt, mock_transport_new_sco(MOCK_DEVICE_2,
					BA_TRANSPORT_PROFILE_HSP_AG, MOCK_BLUEZ_SCO_PATH_2));
	}

	mock_sem_wait(mock_sem_timeout);

	for (i = 0; i < tt->len; i++) {
		usleep(mock_fuzzing_ms * 1000);
		ba_transport_destroy(tt->pdata[i]);
	}

	usleep(mock_fuzzing_ms * 1000);

	g_ptr_array_free(tt, TRUE);
	mock_sem_signal(mock_sem_quit);
	return NULL;
}

void mock_bluealsa_dbus_name_acquired(GDBusConnection *conn, const char *name, void *userdata) {
	(void)conn;
	(void)userdata;

	fprintf(stderr, "BLUEALSA_DBUS_SERVICE_NAME=%s\n", name);

	/* do not generate lots of data */
	config.sbc_quality = SBC_QUALITY_LOW;

	/* initialize codec capabilities */
	a2dp_codecs_init();

	/* emulate dummy test HCI device */
	assert((mock_adapter = ba_adapter_new(MOCK_ADAPTER_ID)) != NULL);

	/* make HCI mSBC-ready */
	mock_adapter->hci.features[2] = LMP_TRSP_SCO;
	mock_adapter->hci.features[3] = LMP_ESCO;

	/* run actual BlueALSA mock thread */
	g_thread_unref(g_thread_new(NULL, mock_bluealsa_service_thread, NULL));

}
