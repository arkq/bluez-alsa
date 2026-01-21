/*
 * mock-bluealsa.c
 * SPDX-FileCopyrightText: 2016-2025 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#include "mock.h"

#if HAVE_CONFIG_H
# include <config.h>
#endif

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
#include "a2dp-aptx.h"
#include "a2dp-aptx-hd.h"
#include "a2dp-faststream.h"
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
#include "codec-sbc.h"
#include "error.h"
#include "hci.h"
#include "hfp.h"
#include "io.h"
#include "midi.h"
#include "ofono.h"
#include "storage.h"
#include "upower.h"
#include "shared/a2dp-codecs.h"
#include "shared/bluetooth.h"
#include "shared/defs.h"
#include "shared/log.h"
#include "shared/rt.h"
#include "utils.h"

#include "inc/sine.inc"
#include "service.h"

typedef struct BlueALSAMockServicePriv {
	/* Public members. */
	BlueALSAMockService p;
	/* Mock adapter and devices. */
	struct ba_adapter * adapter;
	struct ba_device * device_1;
	struct ba_device * device_2;
	/* Other mock services. */
	BlueZMockService * bluez;
	OFonoMockService * ofono;
	UPowerMockService * upower;
} BlueALSAMockServicePriv;

static const a2dp_sbc_t config_sbc_44100_stereo = {
	.sampling_freq = SBC_SAMPLING_FREQ_44100,
	.channel_mode = SBC_CHANNEL_MODE_JOINT_STEREO,
	.block_length = SBC_BLOCK_LENGTH_16,
	.subbands = SBC_SUBBANDS_8,
	.allocation_method = SBC_ALLOCATION_LOUDNESS,
	.min_bitpool = SBC_MIN_BITPOOL,
	.max_bitpool = SBC_MAX_BITPOOL,
};

#if ENABLE_APTX
static const a2dp_aptx_t config_aptx_44100_stereo = {
	.info = A2DP_VENDOR_INFO_INIT(APTX_VENDOR_ID, APTX_CODEC_ID),
	.channel_mode = APTX_CHANNEL_MODE_STEREO,
	.sampling_freq = APTX_SAMPLING_FREQ_44100,
};
#endif

#if ENABLE_APTX_HD
static const a2dp_aptx_hd_t config_aptx_hd_48000_stereo = {
	.aptx.info = A2DP_VENDOR_INFO_INIT(APTX_HD_VENDOR_ID, APTX_HD_CODEC_ID),
	.aptx.channel_mode = APTX_CHANNEL_MODE_STEREO,
	.aptx.sampling_freq = APTX_SAMPLING_FREQ_48000,
};
#endif

#if ENABLE_FASTSTREAM
static const a2dp_faststream_t config_faststream_44100_16000 = {
	.info = A2DP_VENDOR_INFO_INIT(FASTSTREAM_VENDOR_ID, FASTSTREAM_CODEC_ID),
	.direction = FASTSTREAM_DIRECTION_MUSIC | FASTSTREAM_DIRECTION_VOICE,
	.sampling_freq_music = FASTSTREAM_SAMPLING_FREQ_MUSIC_44100,
	.sampling_freq_voice = FASTSTREAM_SAMPLING_FREQ_VOICE_16000,
};
#endif

static void *mock_dec(struct ba_transport_pcm *t_pcm) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pcm_thread_cleanup), t_pcm);

	const unsigned int channels = t_pcm->channels;
	const unsigned int rate = t_pcm->rate;
	struct pollfd fds[1] = {{ t_pcm->pipe[0], POLLIN, 0 }};
	struct asrsync asrs = { .frames = 0 };
	int16_t buffer[1024 * 2];
	size_t x = 0;

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
			asrsync_init(&asrs, rate);

		const size_t samples = ARRAYSIZE(buffer);
		const size_t frames = samples / channels;
		x = snd_pcm_sine_s16_2le(buffer, channels, frames, 146.83 / rate, x);

		io_pcm_scale(t_pcm, buffer, samples);
		if (io_pcm_write(t_pcm, buffer, samples) == -1)
			error("PCM write error: %s", strerror(errno));

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
void *a2dp_fs_dec_thread(struct ba_transport_pcm *t_pcm) { return mock_dec(t_pcm); }
void *sco_dec_thread(struct ba_transport_pcm *t_pcm) { return mock_dec(t_pcm); }

static struct ba_adapter * mock_adapter_new(int dev_id) {
	struct ba_adapter * a;
	if ((a = ba_adapter_new(dev_id)) == NULL)
		return NULL;
	/* make dummy test HCI mSBC-ready */
	a->hci.features[2] = LMP_TRSP_SCO;
	a->hci.features[3] = LMP_ESCO;
	return a;
}

static struct ba_device *mock_device_new(struct ba_adapter *a, const char *address) {

	struct ba_device *d;
	bdaddr_t addr;

	str2ba(address, &addr);
	if ((d = ba_device_new(a, &addr)) == NULL)
		return NULL;

	storage_device_clear(d);
	d->battery.charge = 75;

	return d;
}

static struct ba_transport * mock_transport_new_a2dp(BlueALSAMockServicePriv * self,
		struct ba_device * d, const char * uuid, const struct a2dp_sep * sep,
		const void * configuration) {

	usleep(self->p.fuzzing_ms * 1000);

	char transport_path[128];
	const int index = (strcmp(uuid, BT_UUID_A2DP_SINK) == 0) ? 1 : 2;
	sprintf(transport_path, "%s/sep/fd%u", d->bluez_dbus_path, index);

	g_autoptr(GAsyncQueue) queue = g_async_queue_new();
	mock_bluez_service_device_media_set_configuration(self->bluez,
			d->bluez_dbus_path, transport_path, uuid, sep->config.codec_id,
			configuration, sep->config.caps_size, queue);
	g_async_queue_pop(queue);

	char device[18];
	ba2str(&d->addr, device);
	fprintf(stderr, "BLUEALSA_READY=A2DP:%s:%s\n", device,
			a2dp_codecs_codec_id_to_string(sep->config.codec_id));

	struct ba_transport *t;
	assert((t = ba_transport_lookup(d, transport_path)) != NULL);
	return t;
}

/* SCO acquisition override for testing purposes. */
int transport_acquire_bt_sco(struct ba_transport *t) {

	int bt_fds[2];
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, bt_fds) == 0);

	t->bt_fd = bt_fds[0];
	t->mtu_read = 48;
	t->mtu_write = 48;

	debug("New SCO link: %s: %d", batostr_(&t->d->addr), t->bt_fd);

	g_autoptr(GIOChannel) ch = g_io_channel_unix_raw_new(bt_fds[1]);
	g_autoptr(GSource) watch = g_io_create_watch(ch, G_IO_IN | G_IO_HUP | G_IO_ERR);
	g_source_set_callback(watch, G_SOURCE_FUNC(channel_drain_callback), NULL, NULL);
	g_source_attach(watch, NULL);

	return bt_fds[0];
}

/* SCO dispatcher override for testing purposes. */
error_code_t sco_setup_connection_dispatcher(G_GNUC_UNUSED struct ba_adapter * a) {
	/* In the mock implementation we can not setup SCO dispatcher because
	 * there is no real HCI device to bind SCO sockets to. */
	return ERROR_CODE_OK;
}

static struct ba_transport *mock_transport_new_sco(BlueALSAMockServicePriv * self,
		struct ba_device *d, const char *uuid) {

	usleep(self->p.fuzzing_ms * 1000);

	g_autoptr(GAsyncQueue) queue = g_async_queue_new();
	mock_bluez_service_device_profile_new_connection(self->bluez, d->bluez_dbus_path, uuid, queue);
	g_async_queue_pop(queue);

	struct ba_transport *t;
	assert((t = ba_transport_lookup(d, d->bluez_dbus_path)) != NULL);

	if (t->profile & BA_TRANSPORT_PROFILE_MASK_HFP) {
		t->sco.rfcomm->state = HFP_SLC_CONNECTED;
		t->sco.rfcomm->ag_codecs.cvsd = true;
		t->sco.rfcomm->hf_codecs.cvsd = true;
#if ENABLE_HFP_CODEC_SELECTION
		t->sco.rfcomm->ag_features |= HFP_AG_FEAT_CODEC | HFP_AG_FEAT_ESCO;
		t->sco.rfcomm->hf_features |= HFP_HF_FEAT_CODEC | HFP_HF_FEAT_ESCO;
#endif
#if ENABLE_MSBC
		t->sco.rfcomm->ag_codecs.msbc = true;
		t->sco.rfcomm->hf_codecs.msbc = true;
#endif
#if ENABLE_LC3_SWB
		t->sco.rfcomm->ag_codecs.lc3_swb = true;
		t->sco.rfcomm->hf_codecs.lc3_swb = true;
#endif
	}

	char device[18];
	ba2str(&d->addr, device);
	fprintf(stderr, "BLUEALSA_READY=SCO:%s:%s\n", device,
			hfp_codec_id_to_string(ba_transport_get_codec(t)));

	return t;
}

#if ENABLE_MIDI
static struct ba_transport * mock_transport_new_midi(BlueALSAMockServicePriv * self,
		const char *path) {

	usleep(self->p.fuzzing_ms * 1000);

	struct ba_device *d = ba_device_lookup(self->adapter, &self->adapter->hci.bdaddr);
	struct ba_transport *t = ba_transport_lookup(d, path);

	int fds[2];
	socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, fds);
	ble_midi_encode_set_mtu(&t->midi.ble_encoder, 23);
	/* link read and write ends with each other */
	t->midi.ble_fd_write = fds[1];
	t->midi.ble_fd_notify = fds[0];

	midi_transport_start_watch_ble_midi(t);

	char device[18];
	ba2str(&d->addr, device);
	fprintf(stderr, "BLUEALSA_READY=MIDI:%s\n", device);

	ba_device_unref(d);
	return t;
}
#endif

void mock_bluealsa_service_run(BlueALSAMockService * srv, GAsyncQueue * sync) {
	BlueALSAMockServicePriv * self = (BlueALSAMockServicePriv *)srv;

	/* Wait for profiles to be registered. */
	if (config.profile.a2dp_source || config.profile.a2dp_sink)
		g_async_queue_pop(self->bluez->media_application_ready_queue);
	if (config.profile.hfp_ag)
		g_async_queue_pop(self->bluez->profile_ready_queue);
	if (config.profile.hfp_hf)
		g_async_queue_pop(self->bluez->profile_ready_queue);
	if (config.profile.hsp_ag)
		g_async_queue_pop(self->bluez->profile_ready_queue);
	if (config.profile.hsp_hs)
		g_async_queue_pop(self->bluez->profile_ready_queue);

	/* Create remote SEP on device 1, so we could test SEP configuration. */
	mock_bluez_service_device_add_media_endpoint(self->bluez, MOCK_BLUEZ_DEVICE_1_PATH,
			MOCK_BLUEZ_DEVICE_1_SEP_PATH, BT_UUID_A2DP_SINK, a2dp_sbc_sink.config.codec_id,
			&a2dp_sbc_sink.config.capabilities, a2dp_sbc_sink.config.caps_size);

#if ENABLE_ASHA
	const uint8_t sync_id[8] = { 0xF1, 0x05, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
	/* Create ASHA transport on device 1 for testing ASHA support. */
	mock_bluez_service_device_add_asha_transport(self->bluez, MOCK_BLUEZ_DEVICE_1_PATH,
			MOCK_BLUEZ_DEVICE_1_ASHA_PATH, "right", false, sync_id);
#endif

	GPtrArray *tt = g_ptr_array_new();

	if (config.profile.a2dp_source) {

		if (a2dp_sbc_source.enabled)
			g_ptr_array_add(tt, mock_transport_new_a2dp(self, self->device_1, BT_UUID_A2DP_SOURCE,
						&a2dp_sbc_source, &config_sbc_44100_stereo));

#if ENABLE_APTX
		if (a2dp_aptx_source.enabled)
			g_ptr_array_add(tt, mock_transport_new_a2dp(self, self->device_2, BT_UUID_A2DP_SOURCE,
						&a2dp_aptx_source, &config_aptx_44100_stereo));
		else
#endif
#if ENABLE_APTX_HD
		if (a2dp_aptx_hd_source.enabled)
			g_ptr_array_add(tt, mock_transport_new_a2dp(self, self->device_2, BT_UUID_A2DP_SOURCE,
						&a2dp_aptx_hd_source, &config_aptx_hd_48000_stereo));
		else
#endif
#if ENABLE_FASTSTREAM
		if (a2dp_faststream_source.enabled)
			g_ptr_array_add(tt, mock_transport_new_a2dp(self, self->device_2, BT_UUID_A2DP_SOURCE,
						&a2dp_faststream_source, &config_faststream_44100_16000));
		else
#endif
		if (a2dp_sbc_source.enabled)
			g_ptr_array_add(tt, mock_transport_new_a2dp(self, self->device_2, BT_UUID_A2DP_SOURCE,
						&a2dp_sbc_source, &config_sbc_44100_stereo));

	}

	if (config.profile.a2dp_sink) {

#if ENABLE_APTX
		if (a2dp_aptx_sink.enabled)
			g_ptr_array_add(tt, mock_transport_new_a2dp(self, self->device_1, BT_UUID_A2DP_SINK,
						&a2dp_aptx_sink, &config_aptx_44100_stereo));
		else
#endif
#if ENABLE_APTX_HD
		if (a2dp_aptx_hd_sink.enabled)
			g_ptr_array_add(tt, mock_transport_new_a2dp(self, self->device_1, BT_UUID_A2DP_SINK,
						&a2dp_aptx_hd_sink, &config_aptx_hd_48000_stereo));
		else
#endif
		if (a2dp_sbc_sink.enabled)
			g_ptr_array_add(tt, mock_transport_new_a2dp(self, self->device_1, BT_UUID_A2DP_SINK,
						&a2dp_sbc_sink, &config_sbc_44100_stereo));

		if (a2dp_sbc_sink.enabled)
			g_ptr_array_add(tt, mock_transport_new_a2dp(self, self->device_2, BT_UUID_A2DP_SINK,
						&a2dp_sbc_sink, &config_sbc_44100_stereo));

	}

	if (config.profile.hfp_ag) {

		struct ba_transport *t;
		g_ptr_array_add(tt, t = mock_transport_new_sco(self, self->device_1, BT_UUID_HFP_AG));

		/* In case of fuzzing, select available codecs
		 * one by one with some delay in between. */

		if (self->p.fuzzing_ms) {
			ba_transport_set_codec(t, HFP_CODEC_CVSD);
#if ENABLE_MSBC
			usleep(self->p.fuzzing_ms * 1000);
			ba_transport_set_codec(t, HFP_CODEC_MSBC);
#endif
#if ENABLE_LC3_SWB
			usleep(self->p.fuzzing_ms * 1000);
			ba_transport_set_codec(t, HFP_CODEC_LC3_SWB);
#endif
		}

	}

	if (config.profile.hfp_hf)
		g_ptr_array_add(tt, mock_transport_new_sco(self, self->device_1, BT_UUID_HFP_HF));

	if (config.profile.hsp_ag)
		g_ptr_array_add(tt, mock_transport_new_sco(self, self->device_2, BT_UUID_HSP_AG));

	if (config.profile.hsp_hs)
		g_ptr_array_add(tt, mock_transport_new_sco(self, self->device_2, BT_UUID_HSP_HS));

#if ENABLE_UPOWER
	mock_upower_service_display_device_set_percentage(self->upower, 50.00);
	mock_upower_service_display_device_set_is_present(self->upower, false);
#endif

#if ENABLE_MIDI
	if (config.profile.midi)
		g_ptr_array_add(tt, mock_transport_new_midi(self, MOCK_BLUEZ_MIDI_PATH));
#endif

	g_async_queue_pop(sync);

	for (size_t i = 0; i < tt->len; i++) {
		usleep(self->p.fuzzing_ms * 1000);
		ba_transport_destroy(tt->pdata[i]);
	}

	usleep(self->p.fuzzing_ms * 1000);

	g_ptr_array_free(tt, TRUE);

}

static void name_acquired(G_GNUC_UNUSED GDBusConnection * conn,
		const char * name, void * userdata) {
	BlueALSAMockServicePriv * self = userdata;

	config.dbus = conn;
	/* do not generate lots of data */
	config.sbc_quality = SBC_QUALITY_LOW;
	/* initialize SEPs capabilities */
	a2dp_seps_init();

	/* Create mock devices attached to the mock adapter. */
	self->adapter = mock_adapter_new(MOCK_ADAPTER_ID);
	self->device_1 = mock_device_new(self->adapter, MOCK_DEVICE_1);
	self->device_2 = mock_device_new(self->adapter, MOCK_DEVICE_2);

	/* register D-Bus interfaces */
	bluealsa_dbus_register();
	/* setup BlueZ integration */
	bluez_init();
#if ENABLE_OFONO
	/* setup oFono integration */
	ofono_init();
#endif
#if ENABLE_UPOWER
	/* setup UPower integration */
	upower_init();
#endif

	fprintf(stderr, "BLUEALSA_DBUS_SERVICE_NAME=%s\n", name);
	mock_service_ready(self);
}

static void service_free(void * service) {
	g_autofree BlueALSAMockServicePriv * self = service;
	ba_device_unref(self->device_1);
	ba_device_unref(self->device_2);
	ba_adapter_destroy(self->adapter);
}

BlueALSAMockService * mock_bluealsa_service_new(char * name,
		BlueZMockService * bluez, OFonoMockService * ofono, UPowerMockService * upower) {

	BlueALSAMockServicePriv * self = g_new0(BlueALSAMockServicePriv, 1);
	self->p.service.name = name;
	self->p.service.name_acquired_cb = name_acquired;
	self->p.service.free = service_free;

	self->bluez = bluez;
	self->ofono = ofono;
	self->upower = upower;

	return (BlueALSAMockService *)self;
}
