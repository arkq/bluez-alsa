/*
 * BlueALSA - bluealsa-dbus.c
 * Copyright (c) 2016-2023 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "bluealsa-dbus.h"
/* IWYU pragma: no_include "config.h" */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <bluetooth/hci.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib-object.h>
#include <glib.h>

#include "a2dp.h"
#include "ba-adapter.h"
#include "ba-device.h"
#include "ba-transport.h"
#include "bluealsa-config.h"
#include "bluealsa-iface.h"
#include "bluealsa-skeleton.h"
#include "dbus.h"
#include "hfp.h"
#include "utils.h"
#include "shared/a2dp-codecs.h"
#include "shared/defs.h"
#include "shared/log.h"

static const char *bluealsa_dbus_manager_path = "/org/bluealsa";
static GDBusObjectManagerServer *bluealsa_dbus_manager = NULL;

static GVariant *ba_variant_new_bluealsa_version(void) {
	return g_variant_new_string(PACKAGE_VERSION);
}

static GVariant *ba_variant_new_bluealsa_adapters(void) {

	const char *strv[ARRAYSIZE(config.adapters)];
	GVariant *variant;
	size_t n = 0;

	pthread_mutex_lock(&config.adapters_mutex);

	for (size_t i = 0; i < ARRAYSIZE(config.adapters); i++)
		if (config.adapters[i] != NULL)
			strv[n++] = config.adapters[i]->hci.name;

	variant = g_variant_new_strv(strv, n);

	pthread_mutex_unlock(&config.adapters_mutex);

	return variant;
}

static GVariant *ba_variant_new_bluealsa_profiles(void) {

	struct {
		const char *name;
		bool enabled;
	} profiles[] = {
		{ BLUEALSA_TRANSPORT_TYPE_A2DP_SOURCE, config.profile.a2dp_source },
		{ BLUEALSA_TRANSPORT_TYPE_A2DP_SINK, config.profile.a2dp_sink },
#if ENABLE_OFONO
		{ BLUEALSA_TRANSPORT_TYPE_HFP_OFONO, config.profile.hfp_ofono },
#endif
		{ BLUEALSA_TRANSPORT_TYPE_HFP_AG, config.profile.hfp_ag },
		{ BLUEALSA_TRANSPORT_TYPE_HFP_HF, config.profile.hfp_hf },
		{ BLUEALSA_TRANSPORT_TYPE_HSP_AG, config.profile.hsp_ag },
		{ BLUEALSA_TRANSPORT_TYPE_HSP_HS, config.profile.hsp_hs },
	};

	const char *strv[ARRAYSIZE(profiles)];
	size_t n = 0;

	for (size_t i = 0; i < ARRAYSIZE(profiles); i++)
		if (profiles[i].enabled)
			strv[n++] = profiles[i].name;

	return g_variant_new_strv(strv, n);
}

static GVariant *ba_variant_new_bluealsa_codecs(void) {

	char tmp[64][32];
	const char *strv[ARRAYSIZE(tmp)];
	size_t n = 0;

	const struct a2dp_codec * a2dp_codecs_tmp[32];
	struct a2dp_codec * const * cc = a2dp_codecs;
	for (const struct a2dp_codec *c = *cc; c != NULL; c = *++cc) {
		if (!c->enabled)
			continue;
		a2dp_codecs_tmp[n] = c;
		if (++n >= ARRAYSIZE(a2dp_codecs_tmp))
			break;
	}

	/* Expose A2DP codecs always in the same order. */
	qsort(a2dp_codecs_tmp, n, sizeof(*a2dp_codecs_tmp),
			QSORT_COMPAR(a2dp_codec_ptr_cmp));

	for (size_t i = 0; i < n; i++) {
		const char *profile = a2dp_codecs_tmp[i]->dir == A2DP_SOURCE ?
				BLUEALSA_TRANSPORT_TYPE_A2DP_SOURCE : BLUEALSA_TRANSPORT_TYPE_A2DP_SINK;
		const char *name = a2dp_codecs_codec_id_to_string(a2dp_codecs_tmp[i]->codec_id);
		snprintf(tmp[i], sizeof(tmp[i]), "%s:%s", profile, name);
		strv[i] = (const char *)&tmp[i];
	}

	static const char *hfp_profiles[] = {
#if ENABLE_OFONO
		BLUEALSA_TRANSPORT_TYPE_HFP_OFONO,
# endif
		BLUEALSA_TRANSPORT_TYPE_HFP_AG,
		BLUEALSA_TRANSPORT_TYPE_HFP_HF,
	};

	struct {
		uint16_t codec_id;
		bool enabled;
	} hfp_codecs[] = {
		{ HFP_CODEC_CVSD, config.hfp.codecs.cvsd },
#if ENABLE_MSBC
		{ HFP_CODEC_MSBC, config.hfp.codecs.msbc },
#endif
	};

	for (size_t i = 0; i < ARRAYSIZE(hfp_profiles); i++)
		for (size_t ii = 0; ii < ARRAYSIZE(hfp_codecs); ii++)
			if (hfp_codecs[ii].enabled) {
				const char *name = hfp_codec_id_to_string(hfp_codecs[ii].codec_id);
				snprintf(tmp[n], sizeof(tmp[n]), "%s:%s", hfp_profiles[i], name);
				strv[n] = (const char *)&tmp[n];
				n++;
			}

	static const char *hsp_profiles[] = {
		BLUEALSA_TRANSPORT_TYPE_HSP_AG,
		BLUEALSA_TRANSPORT_TYPE_HSP_HS,
	};

	for (size_t i = 0; i < ARRAYSIZE(hsp_profiles); i++) {
		const char *name = hfp_codec_id_to_string(HFP_CODEC_CVSD);
		snprintf(tmp[n], sizeof(tmp[n]), "%s:%s", hsp_profiles[i], name);
		strv[n] = (const char *)&tmp[n];
		n++;
	}

	return g_variant_new_strv(strv, n);
}

GVariant *ba_variant_new_device_path(const struct ba_device *d) {
	return g_variant_new_object_path(d->bluez_dbus_path);
}

static GVariant *ba_variant_new_device_sequence(const struct ba_device *d) {
	return g_variant_new_uint32(d->seq);
}

GVariant *ba_variant_new_device_battery(const struct ba_device *d) {
	return g_variant_new_byte(d->battery.charge);
}

static GVariant *ba_variant_new_transport_type(const struct ba_transport *t) {
	switch (t->profile) {
	case BA_TRANSPORT_PROFILE_A2DP_SOURCE:
		return g_variant_new_string(BLUEALSA_TRANSPORT_TYPE_A2DP_SOURCE);
	case BA_TRANSPORT_PROFILE_A2DP_SINK:
		return g_variant_new_string(BLUEALSA_TRANSPORT_TYPE_A2DP_SINK);
	case BA_TRANSPORT_PROFILE_HFP_AG:
		return g_variant_new_string(BLUEALSA_TRANSPORT_TYPE_HFP_AG);
	case BA_TRANSPORT_PROFILE_HFP_HF:
		return g_variant_new_string(BLUEALSA_TRANSPORT_TYPE_HFP_HF);
	case BA_TRANSPORT_PROFILE_HSP_AG:
		return g_variant_new_string(BLUEALSA_TRANSPORT_TYPE_HSP_AG);
	case BA_TRANSPORT_PROFILE_HSP_HS:
		return g_variant_new_string(BLUEALSA_TRANSPORT_TYPE_HSP_HS);
	case BA_TRANSPORT_PROFILE_NONE:
		break;
	}
	error("Unsupported transport type: %#x", t->profile);
	g_assert_not_reached();
	return NULL;
}

static GVariant *ba_variant_new_rfcomm_features(const struct ba_rfcomm *r) {
	return g_variant_new_uint32(r->hfp_features);
}

static GVariant *ba_variant_new_pcm_mode(const struct ba_transport_pcm *pcm) {
	if (pcm->mode == BA_TRANSPORT_PCM_MODE_SOURCE)
		return g_variant_new_string(BLUEALSA_PCM_MODE_SOURCE);
	return g_variant_new_string(BLUEALSA_PCM_MODE_SINK);
}

static GVariant *ba_variant_new_pcm_format(const struct ba_transport_pcm *pcm) {
	return g_variant_new_uint16(pcm->format);
}

static GVariant *ba_variant_new_pcm_channels(const struct ba_transport_pcm *pcm) {
	return g_variant_new_byte(pcm->channels);
}

static GVariant *ba_variant_new_pcm_sampling(const struct ba_transport_pcm *pcm) {
	return g_variant_new_uint32(pcm->sampling);
}

static GVariant *ba_variant_new_pcm_codec(const struct ba_transport_pcm *pcm) {
	const struct ba_transport *t = pcm->t;
	const char *codec = NULL;
	if (t->profile & BA_TRANSPORT_PROFILE_MASK_A2DP)
		codec = a2dp_codecs_codec_id_to_string(t->codec_id);
	if (t->profile & BA_TRANSPORT_PROFILE_MASK_SCO)
		codec = hfp_codec_id_to_string(t->codec_id);
	if (codec != NULL)
		return g_variant_new_string(codec);
	return NULL;
}

static GVariant *ba_variant_new_pcm_codec_config(const struct ba_transport_pcm *pcm) {
	const struct ba_transport *t = pcm->t;
	if (t->profile & BA_TRANSPORT_PROFILE_MASK_A2DP)
		return g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, &t->a2dp.configuration,
				t->a2dp.codec->capabilities_size, sizeof(uint8_t));
	return NULL;
}

static GVariant *ba_variant_new_pcm_delay(const struct ba_transport_pcm *pcm) {
	return g_variant_new_uint16(ba_transport_pcm_get_delay(pcm));
}

static GVariant *ba_variant_new_pcm_soft_volume(const struct ba_transport_pcm *pcm) {
	return g_variant_new_boolean(pcm->soft_volume);
}

static uint8_t ba_volume_pack_dbus_volume(bool muted, int value) {
	return (muted << 7) | (((uint8_t)value) & 0x7F);
}

static GVariant *ba_variant_new_pcm_volume(const struct ba_transport_pcm *pcm) {
	uint8_t ch1 = ba_volume_pack_dbus_volume(pcm->volume[0].scale == 0,
			ba_transport_pcm_volume_level_to_bt(pcm, pcm->volume[0].level));
	uint8_t ch2 = ba_volume_pack_dbus_volume(pcm->volume[1].scale == 0,
			ba_transport_pcm_volume_level_to_bt(pcm, pcm->volume[1].level));
	return g_variant_new_uint16((ch1 << 8) | (pcm->channels == 1 ? 0 : ch2));
}

static GVariant *ba_variant_new_pcm_running(const struct ba_transport_pcm *pcm) {
	return g_variant_new_boolean(pcm->th->state == BA_TRANSPORT_THREAD_STATE_RUNNING);
}

static void ba_variant_populate_pcm(GVariantBuilder *props, const struct ba_transport_pcm *pcm) {

	GVariant *value;
	g_variant_builder_init(props, G_VARIANT_TYPE("a{sv}"));

	g_variant_builder_add(props, "{sv}", "Device", ba_variant_new_device_path(pcm->t->d));
	g_variant_builder_add(props, "{sv}", "Sequence", ba_variant_new_device_sequence(pcm->t->d));
	g_variant_builder_add(props, "{sv}", "Transport", ba_variant_new_transport_type(pcm->t));
	g_variant_builder_add(props, "{sv}", "Mode", ba_variant_new_pcm_mode(pcm));
	g_variant_builder_add(props, "{sv}", "Format", ba_variant_new_pcm_format(pcm));
	g_variant_builder_add(props, "{sv}", "Channels", ba_variant_new_pcm_channels(pcm));
	g_variant_builder_add(props, "{sv}", "Sampling", ba_variant_new_pcm_sampling(pcm));
	if ((value = ba_variant_new_pcm_codec(pcm)) != NULL)
		g_variant_builder_add(props, "{sv}", "Codec", value);
	if ((value = ba_variant_new_pcm_codec_config(pcm)) != NULL)
		g_variant_builder_add(props, "{sv}", "CodecConfiguration", value);
	g_variant_builder_add(props, "{sv}", "Delay", ba_variant_new_pcm_delay(pcm));
	g_variant_builder_add(props, "{sv}", "SoftVolume", ba_variant_new_pcm_soft_volume(pcm));
	g_variant_builder_add(props, "{sv}", "Volume", ba_variant_new_pcm_volume(pcm));
	g_variant_builder_add(props, "{sv}", "Running", ba_variant_new_pcm_running(pcm));

}

static bool ba_variant_populate_sep(GVariantBuilder *props, const struct a2dp_sep *sep) {

	const struct a2dp_codec *codec;
	if ((codec = a2dp_codec_lookup(sep->codec_id, !sep->dir)) == NULL)
		return false;

	/* Do not report SEP if corresponding codec is not enabled
	 * in BlueALSA - it will be not possible to use it. */
	if (!codec->enabled)
		return false;

	uint8_t caps[sizeof(sep->capabilities)];
	size_t size = MIN(sep->capabilities_size, sizeof(caps));
	if (a2dp_filter_capabilities(codec, memcpy(caps, &sep->capabilities, size), size) != 0) {
		error("Couldn't filter %s capabilities: %s",
				a2dp_codecs_codec_id_to_string(sep->codec_id),
				strerror(errno));
		return false;
	}

	g_variant_builder_init(props, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(props, "{sv}", "Capabilities", g_variant_new_fixed_array(
				G_VARIANT_TYPE_BYTE, caps, size, sizeof(uint8_t)));

	switch (codec->codec_id) {
	case A2DP_CODEC_SBC:
		break;
#if ENABLE_MPEG
	case A2DP_CODEC_MPEG12:
		break;
#endif
#if ENABLE_AAC
	case A2DP_CODEC_MPEG24:
		break;
#endif
#if ENABLE_APTX
	case A2DP_CODEC_VENDOR_APTX:
		break;
#endif
#if ENABLE_APTX_HD
	case A2DP_CODEC_VENDOR_APTX_HD:
		break;
#endif
#if ENABLE_FASTSTREAM
	case A2DP_CODEC_VENDOR_FASTSTREAM:
		break;
#endif
#if ENABLE_LC3PLUS
	case A2DP_CODEC_VENDOR_LC3PLUS:
		break;
#endif
#if ENABLE_LDAC
	case A2DP_CODEC_VENDOR_LDAC:
		break;
#endif
	default:
		g_assert_not_reached();
	}

	return true;
}

static GVariant *bluealsa_manager_get_property(const char *property,
		GError **error, void *userdata) {
	(void)error;
	(void)userdata;

	if (strcmp(property, "Version") == 0)
		return ba_variant_new_bluealsa_version();
	if (strcmp(property, "Adapters") == 0)
		return ba_variant_new_bluealsa_adapters();
	if (strcmp(property, "Profiles") == 0)
		return ba_variant_new_bluealsa_profiles();
	if (strcmp(property, "Codecs") == 0)
		return ba_variant_new_bluealsa_codecs();

	g_assert_not_reached();
	return NULL;
}

/**
 * Register BlueALSA D-Bus manager interfaces. */
void bluealsa_dbus_register(void) {

	static const GDBusInterfaceSkeletonVTable vtable = {
		.get_property = bluealsa_manager_get_property,
	};

	debug("Registering D-Bus manager: %s", bluealsa_dbus_manager_path);

	bluealsa_ManagerIfaceSkeleton *ifs_manager;
	ifs_manager = bluealsa_manager_iface_skeleton_new(&vtable, NULL, NULL);
	g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(ifs_manager),
			config.dbus, bluealsa_dbus_manager_path, NULL);

	bluealsa_dbus_manager = g_dbus_object_manager_server_new(bluealsa_dbus_manager_path);
	g_dbus_object_manager_server_set_connection(bluealsa_dbus_manager, config.dbus);

}

static gboolean bluealsa_pcm_controller(GIOChannel *ch, GIOCondition condition,
		void *userdata) {
	(void)condition;

	struct ba_transport_pcm *pcm = (struct ba_transport_pcm *)userdata;
	char command[32];
	size_t len;

	switch (g_io_channel_read_chars(ch, command, sizeof(command), &len, NULL)) {
	case G_IO_STATUS_ERROR:
		error("Couldn't read controller channel");
		return TRUE;
	case G_IO_STATUS_NORMAL:
		if (strncmp(command, BLUEALSA_PCM_CTRL_DRAIN, len) == 0) {
			if (pcm->mode == BA_TRANSPORT_PCM_MODE_SINK)
				ba_transport_pcm_drain(pcm);
			g_io_channel_write_chars(ch, "OK", -1, &len, NULL);
		}
		else if (strncmp(command, BLUEALSA_PCM_CTRL_DROP, len) == 0) {
			if (pcm->mode == BA_TRANSPORT_PCM_MODE_SINK)
				ba_transport_pcm_drop(pcm);
			g_io_channel_write_chars(ch, "OK", -1, &len, NULL);
		}
		else if (strncmp(command, BLUEALSA_PCM_CTRL_PAUSE, len) == 0) {
			ba_transport_pcm_pause(pcm);
			g_io_channel_write_chars(ch, "OK", -1, &len, NULL);
		}
		else if (strncmp(command, BLUEALSA_PCM_CTRL_RESUME, len) == 0) {
			ba_transport_pcm_resume(pcm);
			g_io_channel_write_chars(ch, "OK", -1, &len, NULL);
		}
		else {
			warn("Invalid PCM control command: %*s", (int)len, command);
			g_io_channel_write_chars(ch, "Invalid", -1, &len, NULL);
		}
		g_io_channel_flush(ch, NULL);
		return TRUE;
	case G_IO_STATUS_AGAIN:
		return TRUE;
	case G_IO_STATUS_EOF:
		pthread_mutex_lock(&pcm->mutex);
		ba_transport_pcm_release(pcm);
		ba_transport_thread_signal_send(pcm->th, BA_TRANSPORT_THREAD_SIGNAL_PCM_CLOSE);
		pthread_mutex_unlock(&pcm->mutex);
		/* Check whether we've just closed the last PCM client and in
		 * such a case schedule transport IO threads termination. */
		ba_transport_stop_if_no_clients(pcm->t);
		/* remove channel from watch */
		return FALSE;
	}

	return TRUE;
}

static void bluealsa_pcm_open(GDBusMethodInvocation *inv, void *userdata) {

	struct ba_transport_pcm *pcm = (struct ba_transport_pcm *)userdata;
	const bool is_sink = pcm->mode == BA_TRANSPORT_PCM_MODE_SINK;
	struct ba_transport_thread *th = pcm->th;
	struct ba_transport *t = pcm->t;
	int pcm_fds[4] = { -1, -1, -1, -1 };
	size_t i;

	/* Prevent two (or more) clients trying to
	 * open the same PCM at the same time. */
	pthread_mutex_lock(&pcm->client_mtx);

	pthread_mutex_lock(&t->codec_id_mtx);
	const uint16_t codec_id = t->codec_id;
	pthread_mutex_unlock(&t->codec_id_mtx);

	/* preliminary check whether HFP codes is selected */
	if (t->profile & BA_TRANSPORT_PROFILE_MASK_SCO &&
			codec_id == HFP_CODEC_UNDEFINED) {
		g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
				G_DBUS_ERROR_FAILED, "HFP audio codec not selected");
		goto fail;
	}

	pthread_mutex_lock(&pcm->mutex);
	const int pcm_fd = pcm->fd;
	pthread_mutex_unlock(&pcm->mutex);

	if (pcm_fd != -1) {
		g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
				G_DBUS_ERROR_FAILED, "%s", strerror(EBUSY));
		goto fail;
	}

	/* create PCM stream PIPE and PCM control socket */
	if (pipe2(&pcm_fds[0], O_CLOEXEC) == -1 ||
			socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, &pcm_fds[2]) == -1) {
		g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
				G_DBUS_ERROR_FAILED, "Create PIPE: %s", strerror(errno));
		goto fail;
	}

	/* set our internal endpoint as non-blocking. */
	if (fcntl(pcm_fds[is_sink ? 0 : 1], F_SETFL, O_NONBLOCK) == -1) {
		g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
				G_DBUS_ERROR_FAILED, "Setup PIPE: %s", strerror(errno));
		goto fail;
	}

	/* Source profiles (A2DP Source and SCO Audio Gateway) should be initialized
	 * only if the audio is about to be transferred. It is most likely, that BT
	 * headset will not run voltage converter (power-on its circuit board) until
	 * the transport is acquired in order to extend battery life. For profiles
	 * like A2DP Sink and HFP headset, we will wait for incoming connection. */
	if (t->profile & BA_TRANSPORT_PROFILE_A2DP_SOURCE ||
			t->profile & BA_TRANSPORT_PROFILE_MASK_AG) {

		if (ba_transport_acquire(t) == -1) {
			g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
					G_DBUS_ERROR_IO_ERROR, "Acquire transport: %s", strerror(errno));
			goto fail;
		}

		/* Wait until transport thread is ready to process audio. */
		if (ba_transport_thread_state_wait_running(th) == -1) {
			g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
					G_DBUS_ERROR_IO_ERROR, "Acquire transport: %s", strerror(errno));
			goto fail;
		}

	}

	pthread_mutex_lock(&pcm->mutex);
	/* get correct PIPE endpoint - PIPE is unidirectional */
	pcm->fd = pcm_fds[is_sink ? 0 : 1];
	/* set newly opened PCM as active */
	pcm->active = true;
	pthread_mutex_unlock(&pcm->mutex);

	GIOChannel *ch = g_io_channel_unix_new(pcm_fds[2]);
	g_io_channel_set_close_on_unref(ch, TRUE);
	g_io_channel_set_encoding(ch, NULL, NULL);

	g_io_add_watch_full(ch, G_PRIORITY_DEFAULT, G_IO_IN,
			bluealsa_pcm_controller, ba_transport_pcm_ref(pcm),
			(GDestroyNotify)ba_transport_pcm_unref);
	g_io_channel_unref(ch);

	/* notify our audio thread that the FIFO is ready */
	ba_transport_thread_signal_send(th, BA_TRANSPORT_THREAD_SIGNAL_PCM_OPEN);

	int fds[2] = { pcm_fds[is_sink ? 1 : 0], pcm_fds[3] };
	GUnixFDList *fd_list = g_unix_fd_list_new_from_array(fds, 2);
	g_dbus_method_invocation_return_value_with_unix_fd_list(inv,
			g_variant_new("(hh)", 0, 1), fd_list);
	g_object_unref(fd_list);

	pthread_mutex_unlock(&pcm->client_mtx);
	return;

fail:
	pthread_mutex_unlock(&pcm->client_mtx);
	/* clean up created file descriptors */
	for (i = 0; i < ARRAYSIZE(pcm_fds); i++)
		if (pcm_fds[i] != -1)
			close(pcm_fds[i]);
}

static void bluealsa_pcm_get_codecs(GDBusMethodInvocation *inv, void *userdata) {

	struct ba_transport_pcm *pcm = (struct ba_transport_pcm *)userdata;
	const struct ba_transport *t = pcm->t;
	const GArray *seps = t->d->seps;

	GVariantBuilder codecs;
	g_variant_builder_init(&codecs, G_VARIANT_TYPE("a{sa{sv}}"));

	if (t->profile & BA_TRANSPORT_PROFILE_MASK_A2DP) {

		GArray *codec_ids = g_array_sized_new(FALSE, FALSE, sizeof(uint16_t), 16);
		size_t i;

		for (i = 0; seps != NULL && i < seps->len; i++) {
			const struct a2dp_sep *sep = &g_array_index(seps, struct a2dp_sep, i);
			/* match complementary PCM directions, e.g. A2DP-source with SEP-sink */
			if (t->a2dp.codec->dir == !sep->dir) {

				bool duplicate = false;
				size_t j;

				for (j = 0; j < codec_ids->len; j++)
					if (sep->codec_id == g_array_index(codec_ids, uint16_t, j)) {
						duplicate = true;
						break;
					}

				/* do not return duplicates */
				if (duplicate)
					continue;

				g_array_append_val(codec_ids, sep->codec_id);

				GVariantBuilder props;
				if (ba_variant_populate_sep(&props, sep)) {
					g_variant_builder_add(&codecs, "{sa{sv}}",
							a2dp_codecs_codec_id_to_string(sep->codec_id), &props);
					g_variant_builder_clear(&props);
				}

			}
		}

		g_array_free(codec_ids, TRUE);

	}
	else if (t->profile & BA_TRANSPORT_PROFILE_MASK_SCO) {

		if (config.hfp.codecs.cvsd)
			g_variant_builder_add(&codecs, "{sa{sv}}",
					hfp_codec_id_to_string(HFP_CODEC_CVSD), NULL);

#if ENABLE_MSBC
		if (config.hfp.codecs.msbc &&
				t->sco.rfcomm != NULL && t->sco.rfcomm->codecs.msbc)
			g_variant_builder_add(&codecs, "{sa{sv}}",
					hfp_codec_id_to_string(HFP_CODEC_MSBC), NULL);
#endif

	}

	g_dbus_method_invocation_return_value(inv, g_variant_new("(a{sa{sv}})", &codecs));
	g_variant_builder_clear(&codecs);

}

static void bluealsa_pcm_select_codec(GDBusMethodInvocation *inv, void *userdata) {

	GVariant *params = g_dbus_method_invocation_get_parameters(inv);
	struct ba_transport_pcm *pcm = (struct ba_transport_pcm *)userdata;
	struct ba_transport *t = pcm->t;
	GVariantIter *properties;
	GVariant *value = NULL;
	const char *errmsg = NULL;
	const char *codec_name;
	const char *property;

	/* Since transport can provide more than one PCM interface, i.e., source
	 * and sink for bi-directional transports like HSP/HFP. In such case, both
	 * PCMs should use the same codec. Given that, we need to lock codec
	 * selection on the transport level. */
	pthread_mutex_lock(&t->codec_select_client_mtx);

	a2dp_t a2dp_configuration = {};
	size_t a2dp_configuration_size = 0;

	g_variant_get(params, "(&sa{sv})", &codec_name, &properties);
	while (g_variant_iter_next(properties, "{&sv}", &property, &value)) {

		if (strcmp(property, "Configuration") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_BYTESTRING, property)) {

			const void *data = g_variant_get_fixed_array(value,
					&a2dp_configuration_size, sizeof(char));

			if (a2dp_configuration_size > sizeof(a2dp_configuration)) {
				warn("Configuration blob size exceeded: %zu > %zu",
						a2dp_configuration_size, sizeof(a2dp_configuration));
				a2dp_configuration_size = sizeof(a2dp_configuration);
			}

			memcpy(&a2dp_configuration, data, a2dp_configuration_size);

		}

		g_variant_unref(value);
		value = NULL;
	}

	if (t->profile & BA_TRANSPORT_PROFILE_MASK_A2DP) {

		/* support for Stream End-Points not enabled in BlueZ */
		if (t->d->seps == NULL) {
			errmsg = "No BlueZ SEP support";
			goto fail;
		}

		uint16_t codec_id = a2dp_codecs_codec_id_from_string(codec_name);
		enum a2dp_dir dir = !t->a2dp.codec->dir;
		const GArray *seps = t->d->seps;
		struct a2dp_sep *sep = NULL;
		size_t i;

		for (i = 0; i < seps->len; i++)
			if (g_array_index(seps, struct a2dp_sep, i).dir == dir &&
					g_array_index(seps, struct a2dp_sep, i).codec_id == codec_id) {
				sep = &g_array_index(seps, struct a2dp_sep, i);
				break;
			}

		/* requested codec not available */
		if (sep == NULL) {
			errmsg = "SEP codec not available";
			goto fail;
		}

		const struct a2dp_codec *codec;
		if ((codec = a2dp_codec_lookup(codec_id, !dir)) == NULL) {
			errmsg = "SEP codec not supported";
			goto fail;
		}

		/* setup default codec configuration */
		memcpy(&sep->configuration, &sep->capabilities, sep->capabilities_size);
		if (a2dp_select_configuration(codec, &sep->configuration, sep->capabilities_size) == -1)
			goto fail;

		/* use codec configuration blob provided by user */
		if (a2dp_configuration_size != 0) {
			if (a2dp_check_configuration(codec, &a2dp_configuration,
						a2dp_configuration_size) != A2DP_CHECK_OK) {
				errmsg = "Invalid configuration blob";
				goto fail;
			}
			memcpy(&sep->configuration, &a2dp_configuration, sep->capabilities_size);
		}

		if (ba_transport_select_codec_a2dp(t, sep) == -1)
			goto fail;

	}
	else {

		uint16_t codec_id = hfp_codec_id_from_string(codec_name);
		if (ba_transport_select_codec_sco(t, codec_id) == -1)
			goto fail;

	}

	g_dbus_method_invocation_return_value(inv, NULL);
	goto final;

fail:
	if (errmsg == NULL)
		errmsg = strerror(errno);
	error("Couldn't select codec: %s: %s", codec_name, errmsg);
	g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
			G_DBUS_ERROR_FAILED, "%s", errmsg);

final:
	pthread_mutex_unlock(&t->codec_select_client_mtx);
	g_variant_iter_free(properties);
	if (value != NULL)
		g_variant_unref(value);
}

static void bluealsa_rfcomm_open(GDBusMethodInvocation *inv, void *userdata) {

	struct ba_rfcomm *r = (struct ba_rfcomm *)userdata;
	int fds[2] = { -1, -1 };

	if (r->handler_fd != -1) {
		g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
				G_DBUS_ERROR_FAILED, "%s", strerror(EBUSY));
		return;
	}

	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, fds) == -1) {
		g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
				G_DBUS_ERROR_FAILED, "Create socket: %s", strerror(errno));
		return;
	}

	r->handler_fd = fds[0];
	ba_rfcomm_send_signal(r, BA_RFCOMM_SIGNAL_PING);

	GUnixFDList *fd_list = g_unix_fd_list_new_from_array(&fds[1], 1);
	g_dbus_method_invocation_return_value_with_unix_fd_list(inv,
			g_variant_new("(h)", 0), fd_list);
	g_object_unref(fd_list);
}

static GVariant *bluealsa_pcm_get_properties(void *userdata) {

	struct ba_transport_pcm *pcm = (struct ba_transport_pcm *)userdata;

	GVariantBuilder props;

	ba_variant_populate_pcm(&props, pcm);

	return g_variant_builder_end(&props);
}

static GVariant *bluealsa_pcm_get_property(const char *property,
		GError **error, void *userdata) {

	struct ba_transport_pcm *pcm = (struct ba_transport_pcm *)userdata;
	struct ba_device *d = pcm->t->d;
	GVariant *value;

	if (strcmp(property, "Device") == 0)
		return ba_variant_new_device_path(d);
	if (strcmp(property, "Sequence") == 0)
		return ba_variant_new_device_sequence(d);
	if (strcmp(property, "Transport") == 0)
		return ba_variant_new_transport_type(pcm->t);
	if (strcmp(property, "Mode") == 0)
		return ba_variant_new_pcm_mode(pcm);
	if (strcmp(property, "Format") == 0)
		return ba_variant_new_pcm_format(pcm);
	if (strcmp(property, "Channels") == 0)
		return ba_variant_new_pcm_channels(pcm);
	if (strcmp(property, "Sampling") == 0)
		return ba_variant_new_pcm_sampling(pcm);
	if (strcmp(property, "Codec") == 0) {
		if ((value = ba_variant_new_pcm_codec(pcm)) == NULL)
			goto unavailable;
		return value;
	}
	if (strcmp(property, "CodecConfiguration") == 0) {
		if ((value = ba_variant_new_pcm_codec_config(pcm)) == NULL)
			goto unavailable;
		return value;
	}
	if (strcmp(property, "Delay") == 0)
		return ba_variant_new_pcm_delay(pcm);
	if (strcmp(property, "SoftVolume") == 0)
		return ba_variant_new_pcm_soft_volume(pcm);
	if (strcmp(property, "Volume") == 0)
		return ba_variant_new_pcm_volume(pcm);
	if (strcmp(property, "Running") == 0)
		return ba_variant_new_pcm_running(pcm);

	g_assert_not_reached();
	return NULL;

unavailable:
	if (error != NULL)
		*error = g_error_new(G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
				"No such property '%s'", property);
	return NULL;
}

static bool bluealsa_pcm_set_property(const char *property, GVariant *value,
		GError **error, void *userdata) {
	(void)error;

	struct ba_transport_pcm *pcm = (struct ba_transport_pcm *)userdata;

	if (strcmp(property, "SoftVolume") == 0) {
		pcm->soft_volume = g_variant_get_boolean(value);
		bluealsa_dbus_pcm_update(pcm, BA_DBUS_PCM_UPDATE_SOFT_VOLUME);
		return TRUE;
	}

	if (strcmp(property, "Volume") == 0) {

		uint16_t packed = g_variant_get_uint16(value);
		uint8_t ch1 = packed >> 8;
		uint8_t ch2 = packed & 0xFF;

		int ch1_level = ba_transport_pcm_volume_bt_to_level(pcm, ch1 & 0x7F);
		bool ch1_muted = !!(ch1 & 0x80);
		int ch2_level = ba_transport_pcm_volume_bt_to_level(pcm, ch2 & 0x7F);
		bool ch2_muted = !!(ch2 & 0x80);

		pthread_mutex_lock(&pcm->mutex);
		ba_transport_pcm_volume_set(&pcm->volume[0], &ch1_level, &ch1_muted, NULL);
		ba_transport_pcm_volume_set(&pcm->volume[1], &ch2_level, &ch2_muted, NULL);
		pthread_mutex_unlock(&pcm->mutex);

		debug("Setting volume: %u [%.2f dB] %c%c %u [%.2f dB]",
				ch1 & 0x7F, 0.01 * ch1_level, ch1_muted ? 'x' : '<',
				ch2_muted ? 'x' : '>', ch2 & 0x7F, 0.01 * ch2_level);

		ba_transport_pcm_volume_update(pcm);
		return TRUE;
	}

	g_assert_not_reached();
	return FALSE;
}

/**
 * Register BlueALSA D-Bus PCM interface. */
int bluealsa_dbus_pcm_register(struct ba_transport_pcm *pcm) {

	static const GDBusMethodCallDispatcher dispatchers[] = {
		{ .method = "Open",
			.handler = bluealsa_pcm_open },
		{ .method = "GetCodecs",
			.handler = bluealsa_pcm_get_codecs },
		{ .method = "SelectCodec",
			.handler = bluealsa_pcm_select_codec },
		{ 0 },
	};

	static const GDBusInterfaceSkeletonVTable vtable = {
		.dispatchers = dispatchers,
		.get_properties = bluealsa_pcm_get_properties,
		.get_property = bluealsa_pcm_get_property,
		.set_property = bluealsa_pcm_set_property,
	};

	GDBusObjectSkeleton *skeleton = NULL;
	bluealsa_PCMIfaceSkeleton *ifs_pcm = NULL;

	if ((skeleton = g_dbus_object_skeleton_new(pcm->ba_dbus_path)) == NULL)
		goto fail;

	if ((ifs_pcm = bluealsa_pcm_iface_skeleton_new(&vtable,
					pcm, (GDestroyNotify)ba_transport_pcm_unref)) == NULL)
		goto fail;

	ba_transport_pcm_ref(pcm);

	g_dbus_object_skeleton_add_interface(skeleton, G_DBUS_INTERFACE_SKELETON(ifs_pcm));
	g_dbus_object_manager_server_export(bluealsa_dbus_manager, skeleton);
	pcm->ba_dbus_exported = true;

fail:

	if (skeleton != NULL)
		g_object_unref(skeleton);
	if (ifs_pcm != NULL)
		g_object_unref(ifs_pcm);

	return 0;
}

void bluealsa_dbus_pcm_update(struct ba_transport_pcm *pcm, unsigned int mask) {

	GVariantBuilder props;
	g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));

	if (mask & BA_DBUS_PCM_UPDATE_FORMAT)
		g_variant_builder_add(&props, "{sv}", "Format", ba_variant_new_pcm_format(pcm));
	if (mask & BA_DBUS_PCM_UPDATE_CHANNELS)
		g_variant_builder_add(&props, "{sv}", "Channels", ba_variant_new_pcm_channels(pcm));
	if (mask & BA_DBUS_PCM_UPDATE_SAMPLING)
		g_variant_builder_add(&props, "{sv}", "Sampling", ba_variant_new_pcm_sampling(pcm));
	if (mask & BA_DBUS_PCM_UPDATE_CODEC)
		g_variant_builder_add(&props, "{sv}", "Codec", ba_variant_new_pcm_codec(pcm));
	if (mask & BA_DBUS_PCM_UPDATE_CODEC_CONFIG)
		g_variant_builder_add(&props, "{sv}", "CodecConfiguration", ba_variant_new_pcm_codec_config(pcm));
	if (mask & BA_DBUS_PCM_UPDATE_DELAY)
		g_variant_builder_add(&props, "{sv}", "Delay", ba_variant_new_pcm_delay(pcm));
	if (mask & BA_DBUS_PCM_UPDATE_SOFT_VOLUME)
		g_variant_builder_add(&props, "{sv}", "SoftVolume", ba_variant_new_pcm_soft_volume(pcm));
	if (mask & BA_DBUS_PCM_UPDATE_VOLUME)
		g_variant_builder_add(&props, "{sv}", "Volume", ba_variant_new_pcm_volume(pcm));
	if (mask & BA_DBUS_PCM_UPDATE_RUNNING)
		g_variant_builder_add(&props, "{sv}", "Running", ba_variant_new_pcm_running(pcm));

	g_dbus_connection_emit_properties_changed(config.dbus,
			pcm->ba_dbus_path, BLUEALSA_IFACE_PCM, &props, NULL);
	g_variant_builder_clear(&props);

}

void bluealsa_dbus_pcm_unregister(struct ba_transport_pcm *pcm) {

	if (!pcm->ba_dbus_exported)
		return;

	g_dbus_object_manager_server_unexport(bluealsa_dbus_manager, pcm->ba_dbus_path);
	pcm->ba_dbus_exported = false;

}

static GVariant *bluealsa_rfcomm_get_properties(void *userdata) {

	struct ba_rfcomm *r = (struct ba_rfcomm *)userdata;
	struct ba_transport *t = r->sco;
	struct ba_device *d = t->d;

	GVariantBuilder props;
	g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));

	g_variant_builder_add(&props, "{sv}", "Transport", ba_variant_new_transport_type(t));
	g_variant_builder_add(&props, "{sv}", "Features", ba_variant_new_rfcomm_features(r));
	g_variant_builder_add(&props, "{sv}", "Battery", ba_variant_new_device_battery(d));

	return g_variant_builder_end(&props);
}

static GVariant *bluealsa_rfcomm_get_property(const char *property,
		GError **error, void *userdata) {
	(void)error;

	struct ba_rfcomm *r = (struct ba_rfcomm *)userdata;
	struct ba_transport *t = r->sco;
	struct ba_device *d = t->d;

	if (strcmp(property, "Transport") == 0)
		return ba_variant_new_transport_type(t);
	if (strcmp(property, "Features") == 0)
		return ba_variant_new_rfcomm_features(r);
	if (strcmp(property, "Battery") == 0)
		return ba_variant_new_device_battery(d);

	g_assert_not_reached();
	return NULL;
}

/**
 * Register BlueALSA D-Bus RFCOMM interface. */
int bluealsa_dbus_rfcomm_register(struct ba_rfcomm *r) {

	static const GDBusMethodCallDispatcher dispatchers[] = {
		{ .method = "Open",
			.handler = bluealsa_rfcomm_open },
		{ 0 },
	};

	static const GDBusInterfaceSkeletonVTable vtable = {
		.dispatchers = dispatchers,
		.get_properties = bluealsa_rfcomm_get_properties,
		.get_property = bluealsa_rfcomm_get_property,
	};

	GDBusObjectSkeleton *skeleton = NULL;
	bluealsa_RFCOMMIfaceSkeleton *ifs_rfcomm = NULL;

	if ((skeleton = g_dbus_object_skeleton_new(r->ba_dbus_path)) == NULL)
		goto fail;

	if ((ifs_rfcomm = bluealsa_rfcomm_iface_skeleton_new(&vtable,
					r, NULL)) == NULL)
		goto fail;

	g_dbus_object_skeleton_add_interface(skeleton, G_DBUS_INTERFACE_SKELETON(ifs_rfcomm));
	g_dbus_object_manager_server_export(bluealsa_dbus_manager, skeleton);
	r->ba_dbus_exported = true;

fail:

	if (skeleton != NULL)
		g_object_unref(skeleton);
	if (ifs_rfcomm != NULL)
		g_object_unref(ifs_rfcomm);

	return 0;
}

void bluealsa_dbus_rfcomm_update(struct ba_rfcomm *r, unsigned int mask) {

	GVariantBuilder props;
	g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));

	if (mask & BA_DBUS_RFCOMM_UPDATE_FEATURES)
		g_variant_builder_add(&props, "{sv}", "Features", ba_variant_new_rfcomm_features(r));
	if (mask & BA_DBUS_RFCOMM_UPDATE_BATTERY)
		g_variant_builder_add(&props, "{sv}", "Battery", ba_variant_new_device_battery(r->sco->d));

	g_dbus_connection_emit_properties_changed(config.dbus,
			r->ba_dbus_path, BLUEALSA_IFACE_RFCOMM, &props, NULL);
	g_variant_builder_clear(&props);

}

void bluealsa_dbus_rfcomm_unregister(struct ba_rfcomm *r) {
	if (!r->ba_dbus_exported)
		return;
	g_dbus_object_manager_server_unexport(bluealsa_dbus_manager, r->ba_dbus_path);
	r->ba_dbus_exported = false;
}
