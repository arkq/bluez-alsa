/*
 * BlueALSA - bluealsa-dbus.c
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "bluealsa-dbus.h"

#if HAVE_CONFIG_H
# include <config.h>
#endif

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
#include "ba-config.h"
#include "ba-device.h"
#include "ba-transport.h"
#include "ba-transport-pcm.h"
#include "bluealsa-iface.h"
#include "bluez.h"
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

	const struct {
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
#if ENABLE_MIDI
		{ BLUEALSA_TRANSPORT_TYPE_MIDI, config.profile.midi },
#endif
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

	const struct a2dp_sep * a2dp_seps_tmp[32];
	struct a2dp_sep * const * seps = a2dp_seps;
	for (const struct a2dp_sep *sep = *seps; sep != NULL; sep = *++seps) {
		if (!sep->enabled)
			continue;
		a2dp_seps_tmp[n] = sep;
		if (++n >= ARRAYSIZE(a2dp_seps_tmp))
			break;
	}

	/* Expose A2DP codecs always in the same order. */
	qsort(a2dp_seps_tmp, n, sizeof(*a2dp_seps_tmp),
			QSORT_COMPAR(a2dp_sep_ptr_cmp));

	for (size_t i = 0; i < n; i++) {
		const char *profile = a2dp_seps_tmp[i]->config.type == A2DP_SOURCE ?
				BLUEALSA_TRANSPORT_TYPE_A2DP_SOURCE : BLUEALSA_TRANSPORT_TYPE_A2DP_SINK;
		const char *name = a2dp_codecs_codec_id_to_string(a2dp_seps_tmp[i]->config.codec_id);
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

	const struct {
		uint8_t codec_id;
		bool enabled;
	} hfp_codecs[] = {
		{ HFP_CODEC_CVSD, config.hfp.codecs.cvsd },
#if ENABLE_MSBC
		{ HFP_CODEC_MSBC, config.hfp.codecs.msbc },
#endif
#if ENABLE_LC3_SWB
		{ HFP_CODEC_LC3_SWB, config.hfp.codecs.lc3_swb },
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
#if ENABLE_MIDI
	case BA_TRANSPORT_PROFILE_MIDI:
		return g_variant_new_string(BLUEALSA_TRANSPORT_TYPE_MIDI);
#endif
	case BA_TRANSPORT_PROFILE_NONE:
		break;
	}
	error("Unsupported transport type: %#x", t->profile);
	g_assert_not_reached();
	return NULL;
}

static GVariant *ba_variant_new_rfcomm_features(const struct ba_rfcomm *r) {

	const char *strv[32];
	size_t n = 0;

	if (r->sco->profile & BA_TRANSPORT_PROFILE_HFP_AG)
		n = hfp_hf_features_to_strings(r->hf_features, strv, ARRAYSIZE(strv));
	if (r->sco->profile & BA_TRANSPORT_PROFILE_HFP_HF)
		n = hfp_ag_features_to_strings(r->ag_features, strv, ARRAYSIZE(strv));

	return g_variant_new_strv(strv, n);
}

static GVariant *ba_variant_new_pcm_mode(const struct ba_transport_pcm *pcm) {
	if (pcm->mode == BA_TRANSPORT_PCM_MODE_SOURCE)
		return g_variant_new_string(BLUEALSA_PCM_MODE_SOURCE);
	return g_variant_new_string(BLUEALSA_PCM_MODE_SINK);
}

static GVariant *ba_variant_new_pcm_running(const struct ba_transport_pcm *pcm) {
	return g_variant_new_boolean(ba_transport_pcm_state_check_running(pcm));
}

static GVariant *ba_variant_new_pcm_format(const struct ba_transport_pcm *pcm) {
	return g_variant_new_uint16(pcm->format);
}

static GVariant *ba_variant_new_pcm_channels(const struct ba_transport_pcm *pcm) {
	return g_variant_new_byte(pcm->channels);
}

static GVariant *ba_variant_new_pcm_channel_map(const struct ba_transport_pcm *pcm) {

	const char *strv[ARRAYSIZE(pcm->channel_map)];
	const size_t n = pcm->channels;

	for (size_t i = 0; i < n; i++)
		strv[i] = ba_transport_pcm_channel_to_string(pcm->channel_map[i]);

	return g_variant_new_strv(strv, n);
}

static GVariant *ba_variant_new_pcm_rate(const struct ba_transport_pcm *pcm) {
	return g_variant_new_uint32(pcm->rate);
}

static GVariant *ba_variant_new_pcm_codec(const struct ba_transport_pcm *pcm) {
	const struct ba_transport *t = pcm->t;
	const char *codec = NULL;
	if (t->profile & BA_TRANSPORT_PROFILE_MASK_A2DP)
		codec = a2dp_codecs_codec_id_to_string(ba_transport_get_codec(t));
	if (t->profile & BA_TRANSPORT_PROFILE_MASK_SCO)
		codec = hfp_codec_id_to_string(ba_transport_get_codec(t));
	if (codec != NULL)
		return g_variant_new_string(codec);
	return NULL;
}

static GVariant *ba_variant_new_pcm_codec_config(const struct ba_transport_pcm *pcm) {
	const struct ba_transport *t = pcm->t;
	if (t->profile & BA_TRANSPORT_PROFILE_MASK_A2DP)
		return g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, &t->media.configuration,
				t->media.sep->config.caps_size, sizeof(uint8_t));
	return NULL;
}

static GVariant *ba_variant_new_pcm_delay(const struct ba_transport_pcm *pcm) {
	return g_variant_new_uint16(ba_transport_pcm_delay_get(pcm));
}

static GVariant *ba_variant_new_pcm_client_delay(const struct ba_transport_pcm *pcm) {
	return g_variant_new_int16(pcm->client_delay_dms);
}

static GVariant *ba_variant_new_pcm_soft_volume(const struct ba_transport_pcm *pcm) {
	return g_variant_new_boolean(pcm->soft_volume);
}

static uint8_t ba_volume_pack_dbus_volume(bool muted, int value) {
	return (muted << 7) | (((uint8_t)value) & 0x7F);
}

static GVariant *ba_variant_new_pcm_volume(const struct ba_transport_pcm *pcm) {

	const bool is_sco = pcm->t->profile & BA_TRANSPORT_PROFILE_MASK_SCO;
	const int max = is_sco ? HFP_VOLUME_GAIN_MAX : BLUEZ_A2DP_VOLUME_MAX;
	const size_t n = pcm->channels;

	uint8_t volume[ARRAYSIZE(pcm->volume)];
	for (size_t i = 0; i < n; i++)
		volume[i] = ba_volume_pack_dbus_volume(pcm->volume[i].scale == 0,
				ba_transport_pcm_volume_level_to_range(pcm->volume[i].level, max));

	return g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, volume, n, sizeof(*volume));
}

struct ba_populate_data {
	GVariantBuilder *builder;
	/* previously added value */
	unsigned int value;
};

static int ba_populate_channels(struct a2dp_bit_mapping mapping, void *userdata) {
	struct ba_populate_data *data = userdata;
	if (data->value == mapping.value)
		return 0;
	g_variant_builder_add_value(data->builder, g_variant_new_byte(mapping.value));
	data->value = mapping.value;
	return 0;
}

static int ba_populate_rates(struct a2dp_bit_mapping mapping, void *userdata) {
	struct ba_populate_data *data = userdata;
	g_variant_builder_add_value(data->builder, g_variant_new_uint32(mapping.value));
	return 0;
}

static int ba_populate_channel_map(struct a2dp_bit_mapping mapping, void *userdata) {
	struct ba_populate_data *data = userdata;

	if (data->value == mapping.ch.channels)
		return 0;

	const char *strv[16];
	for (size_t i = 0; i < mapping.ch.channels; i++)
		strv[i] = ba_transport_pcm_channel_to_string(mapping.ch.map[i]);

	g_variant_builder_add_value(data->builder, g_variant_new_strv(strv, mapping.ch.channels));
	data->value = mapping.ch.channels;
	return 0;
}

/**
 * Populate dict variant builder with remote SEP properties. */
static void ba_variant_populate_remote_sep(GVariantBuilder *props,
		const struct a2dp_sep *sep, const struct a2dp_sep_config *remote_sep_cfg,
		enum a2dp_stream stream) {

	GVariantBuilder builder;
	struct ba_populate_data data = { .builder = &builder };

	a2dp_t caps = remote_sep_cfg->capabilities;
	sep->caps_helpers->intersect(&caps, &sep->config.capabilities);

	g_variant_builder_add(props, "{sv}", "Capabilities", g_variant_new_fixed_array(
				G_VARIANT_TYPE_BYTE, &caps, remote_sep_cfg->caps_size, sizeof(uint8_t)));

	data.value = 0;
	g_variant_builder_init(&builder, G_VARIANT_TYPE("ay"));
	sep->caps_helpers->foreach_channel_mode(&caps, stream, ba_populate_channels, &data);
	g_variant_builder_add(props, "{sv}", "Channels", g_variant_builder_end(&builder));

	data.value = 0;
	g_variant_builder_init(&builder, G_VARIANT_TYPE("aas"));
	sep->caps_helpers->foreach_channel_mode(&caps, stream, ba_populate_channel_map, &data);
	g_variant_builder_add(props, "{sv}", "ChannelMaps", g_variant_builder_end(&builder));

	data.value = 0;
	g_variant_builder_init(&builder, G_VARIANT_TYPE("au"));
	sep->caps_helpers->foreach_sample_rate(&caps, stream, ba_populate_rates, &data);
	g_variant_builder_add(props, "{sv}", "Rates", g_variant_builder_end(&builder));

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

	debug("Registering BlueALSA D-Bus manager: %s", bluealsa_dbus_manager_path);

	OrgBluealsaManager1Skeleton *ifs_manager;
	ifs_manager = org_bluealsa_manager1_skeleton_new(&vtable, NULL, NULL);
	g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(ifs_manager),
			config.dbus, bluealsa_dbus_manager_path, NULL);

	bluealsa_dbus_manager = g_dbus_object_manager_server_new(bluealsa_dbus_manager_path);
	g_dbus_object_manager_server_set_connection(bluealsa_dbus_manager, config.dbus);

}

static gboolean bluealsa_pcm_controller(GIOChannel *ch, GIOCondition condition,
		void *userdata) {
	(void)condition;

	struct ba_transport_pcm *pcm = userdata;
	GError *err = NULL;
	char command[32];
	size_t len;

	switch (g_io_channel_read_chars(ch, command, sizeof(command), &len, &err)) {
	case G_IO_STATUS_AGAIN:
		return TRUE;
	case G_IO_STATUS_ERROR:
		error("PCM controller read error: %s", err->message);
		g_error_free(err);
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
	case G_IO_STATUS_EOF:
		pthread_mutex_lock(&pcm->mutex);
		ba_transport_pcm_release(pcm);
		pthread_mutex_unlock(&pcm->mutex);
		ba_transport_pcm_signal_send(pcm, BA_TRANSPORT_PCM_SIGNAL_CLOSE);
		/* Check whether we've just closed the last PCM client and in
		 * such a case schedule transport IO threads termination. */
		ba_transport_stop_if_no_clients(pcm->t);
		/* remove channel from watch */
		return FALSE;
	}

	return TRUE;
}

static void bluealsa_pcm_open(GDBusMethodInvocation *inv, void *userdata) {

	struct ba_transport_pcm *pcm = userdata;
	const bool is_sink = pcm->mode == BA_TRANSPORT_PCM_MODE_SINK;
	const enum ba_transport_profile t_profile = pcm->t->profile;
	struct ba_transport *t = pcm->t;
	int pcm_fds[4] = { -1, -1, -1, -1 };

	/* Prevent two (or more) clients trying to
	 * open the same PCM at the same time. */
	pthread_mutex_lock(&pcm->client_mtx);

	/* preliminary check whether HFP codes is selected */
	if (t_profile & BA_TRANSPORT_PROFILE_MASK_SCO &&
			ba_transport_get_codec(t) == HFP_CODEC_UNDEFINED) {
		g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
				G_DBUS_ERROR_FAILED, "HFP audio codec not selected");
		goto fail;
	}

	pthread_mutex_lock(&pcm->mutex);
	const int pcm_fd = pcm->fd;
	pthread_mutex_unlock(&pcm->mutex);

	if (pcm_fd != -1) {
		g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
				G_DBUS_ERROR_LIMITS_EXCEEDED, "%s", strerror(EBUSY));
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
	if (t_profile & BA_TRANSPORT_PROFILE_A2DP_SOURCE ||
			t_profile & BA_TRANSPORT_PROFILE_MASK_AG) {

		if (ba_transport_acquire(t) == -1) {
			g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
					G_DBUS_ERROR_IO_ERROR, "Acquire transport: %s", strerror(errno));
			goto fail;
		}

		/* Wait until transport thread is ready to process audio. */
		if (ba_transport_pcm_state_wait_running(pcm) == -1) {
			g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
					G_DBUS_ERROR_IO_ERROR, "Acquire transport: %s", strerror(errno));
			goto fail;
		}

	}

	pthread_mutex_lock(&pcm->mutex);

	/* get correct PIPE endpoint - PIPE is unidirectional */
	pcm->fd = pcm_fds[is_sink ? 0 : 1];
	/* set newly opened PCM as active */
	pcm->paused = false;

	GIOChannel *ch = g_io_channel_unix_new(pcm_fds[2]);
	g_io_channel_set_close_on_unref(ch, TRUE);
	g_io_channel_set_encoding(ch, NULL, NULL);
	g_io_channel_set_buffered(ch, FALSE);

	pcm->controller = g_io_create_watch_full(ch, G_PRIORITY_DEFAULT,
			G_IO_IN, bluealsa_pcm_controller, ba_transport_pcm_ref(pcm),
			(GDestroyNotify)ba_transport_pcm_unref);
	g_io_channel_unref(ch);

	pthread_mutex_unlock(&pcm->mutex);

	/* notify our PCM IO thread that the PCM was opened */
	ba_transport_pcm_signal_send(pcm, BA_TRANSPORT_PCM_SIGNAL_OPEN);

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
	for (size_t i = 0; i < ARRAYSIZE(pcm_fds); i++)
		if (pcm_fds[i] != -1)
			close(pcm_fds[i]);
}

static void bluealsa_pcm_get_codecs(GDBusMethodInvocation *inv, void *userdata) {

	struct ba_transport_pcm *pcm = userdata;
	const struct ba_transport *t = pcm->t;
	const GArray *sep_cfgs = t->d->sep_configs;

	GVariantBuilder codecs;
	g_variant_builder_init(&codecs, G_VARIANT_TYPE("a{sa{sv}}"));

	if (t->profile & BA_TRANSPORT_PROFILE_MASK_A2DP) {

		GArray *codec_ids = g_array_sized_new(FALSE, FALSE, sizeof(uint32_t), 16);
		const enum a2dp_type pcm_sep_type = t->media.sep->config.type;
		const enum a2dp_stream pcm_sep_stream = &t->media.pcm == pcm ? A2DP_MAIN : A2DP_BACKCHANNEL;

		for (size_t i = 0; sep_cfgs != NULL && i < sep_cfgs->len; i++) {
			const struct a2dp_sep_config *remote_sep_cfg = &ba_device_sep_cfg_array_index(sep_cfgs, i);

			/* Match complementary SEP types (i.e.: source with sink). */
			if (pcm_sep_type == remote_sep_cfg->type)
				continue;

			const struct a2dp_sep *sep;
			/* Find local SEP for the remote one. */
			if ((sep = a2dp_sep_lookup(pcm_sep_type, remote_sep_cfg->codec_id)) == NULL)
				continue;

			/* Do not report codec if corresponding local SEP is not enabled
			 * in BlueALSA - it will be impossible to use it. */
			if (!sep->enabled)
				continue;

			/* Check whether matched local and remote SEP support the same stream
			 * direction as our current PCM SEP. If not, skip this codec. */
			if (!sep->caps_helpers->has_stream(&sep->config.capabilities, pcm_sep_stream) ||
					!sep->caps_helpers->has_stream(&remote_sep_cfg->capabilities, pcm_sep_stream))
				continue;

			bool duplicate = false;
			/* Check whether we have already reported this codec. */
			for (size_t j = 0; j < codec_ids->len; j++)
				if (remote_sep_cfg->codec_id == g_array_index(codec_ids, uint32_t, j)) {
					duplicate = true;
					break;
				}

			/* Do not return duplicates.
			 * Be aware of caveats - codec with the same ID might have different
			 * capabilities... */
			if (duplicate)
				continue;

			GVariantBuilder props;
			g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));

			ba_variant_populate_remote_sep(&props, sep, remote_sep_cfg, pcm_sep_stream);

			g_variant_builder_add(&codecs, "{sa{sv}}",
					a2dp_codecs_codec_id_to_string(remote_sep_cfg->codec_id), &props);
			g_variant_builder_clear(&props);

			/* Remember reported codec ID. */
			g_array_append_val(codec_ids, remote_sep_cfg->codec_id);

		}

		g_array_free(codec_ids, TRUE);

	}
	else if (t->profile & BA_TRANSPORT_PROFILE_MASK_SCO) {

		const struct ba_rfcomm *t_sco_rfcomm = t->sco.rfcomm;

		/* HFP codec is selected by the AG. Because of that, HF is not aware of
		 * AG supported codecs until the codec is actually selected. Anyway, we
		 * will try to provide some heuristic here.
		 * For built-in HFP profiles we will mark given codec as available, if
		 * both AG and HF can support it. When HFP is provided by an external
		 * application like oFono, we will mark given codec as available, if it
		 * is enabled by our global configuration. */

		const struct {
			uint8_t codec_id;
			unsigned int rate;
			bool is_enabled_in_config;
			bool is_available_in_rfcomm_ag;
			bool is_available_in_rfcomm_hf;
		} sco_codecs[] = {
			{ HFP_CODEC_CVSD, 8000, config.hfp.codecs.cvsd,
				t_sco_rfcomm == NULL || t_sco_rfcomm->ag_codecs.cvsd,
				t_sco_rfcomm == NULL || t_sco_rfcomm->hf_codecs.cvsd },
#if ENABLE_MSBC
			{ HFP_CODEC_MSBC, 16000, config.hfp.codecs.msbc,
				t_sco_rfcomm == NULL || t_sco_rfcomm->ag_codecs.msbc,
				t_sco_rfcomm == NULL || t_sco_rfcomm->hf_codecs.msbc },
#endif
#if ENABLE_LC3_SWB
			{ HFP_CODEC_LC3_SWB, 32000, config.hfp.codecs.lc3_swb,
				t_sco_rfcomm == NULL || t_sco_rfcomm->ag_codecs.lc3_swb,
				t_sco_rfcomm == NULL || t_sco_rfcomm->hf_codecs.lc3_swb },
#endif
		};

		for (size_t i = 0; i < ARRAYSIZE(sco_codecs); i++)
			if (sco_codecs[i].is_enabled_in_config &&
					sco_codecs[i].is_available_in_rfcomm_ag &&
					sco_codecs[i].is_available_in_rfcomm_hf) {

				GVariantBuilder props;
				g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));

				const uint8_t channels[] = { 1 };
				g_variant_builder_add(&props, "{sv}", "Channels", g_variant_new_fixed_array(
							G_VARIANT_TYPE_BYTE, channels, 1, sizeof(*channels)));
				const uint32_t rates[] = { sco_codecs[i].rate };
				g_variant_builder_add(&props, "{sv}", "Rates", g_variant_new_fixed_array(
							G_VARIANT_TYPE_UINT32, rates, 1, sizeof(*rates)));

				g_variant_builder_add(&codecs, "{sa{sv}}",
						hfp_codec_id_to_string(sco_codecs[i].codec_id), &props);
				g_variant_builder_clear(&props);

			}

	}

	g_dbus_method_invocation_return_value(inv, g_variant_new("(a{sa{sv}})", &codecs));
	g_variant_builder_clear(&codecs);

}

static void bluealsa_pcm_select_codec(GDBusMethodInvocation *inv, void *userdata) {

	GVariant *params = g_dbus_method_invocation_get_parameters(inv);
	struct ba_transport_pcm *pcm = userdata;
	struct ba_transport *t = pcm->t;
	GVariantIter *properties;
	GVariant *value;
	const char *errmsg = NULL;
	const char *codec_name;
	const char *property;

	/* Since transport can provide more than one PCM interface, i.e., source
	 * and sink for bi-directional transports like HSP/HFP. In such case, both
	 * PCMs should use the same codec. Given that, we need to lock codec
	 * selection on the transport level. */
	pthread_mutex_lock(&t->codec_select_client_mtx);

	a2dp_t a2dp_configuration = { 0 };
	size_t a2dp_configuration_size = 0;
	unsigned int channels = 0;
	unsigned int rate = 0;
	bool conformance_check = true;

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
		else if (strcmp(property, "Channels") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_BYTE, property)) {
			channels = g_variant_get_byte(value);
		}
		else if (strcmp(property, "Rate") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_UINT32, property)) {
			rate = g_variant_get_uint32(value);
		}
		else if (strcmp(property, "NonConformant") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_BOOLEAN, property)) {
			conformance_check = !g_variant_get_boolean(value);
		}

		g_variant_unref(value);
	}

	if (t->profile & BA_TRANSPORT_PROFILE_MASK_A2DP) {

		/* support for Stream End-Points not enabled in BlueZ */
		if (t->d->sep_configs == NULL) {
			errmsg = "No BlueZ SEP support";
			goto fail;
		}

		uint32_t codec_id = a2dp_codecs_codec_id_from_string(codec_name);
		const enum a2dp_type pcm_sep_type = t->media.sep->config.type;
		const enum a2dp_stream pcm_sep_stream = &t->media.pcm == pcm ? A2DP_MAIN : A2DP_BACKCHANNEL;
		struct a2dp_sep_config *remote_sep_cfg = NULL;
		const GArray *sep_cfgs = t->d->sep_configs;

		for (size_t i = 0; i < sep_cfgs->len; i++)
			if (ba_device_sep_cfg_array_index(sep_cfgs, i).type != pcm_sep_type &&
					ba_device_sep_cfg_array_index(sep_cfgs, i).codec_id == codec_id) {
				remote_sep_cfg = &ba_device_sep_cfg_array_index(sep_cfgs, i);
				break;
			}

		/* requested codec not available */
		if (remote_sep_cfg == NULL) {
			errmsg = "SEP codec not available";
			goto fail;
		}

		const struct a2dp_sep *sep;
		if ((sep = a2dp_sep_lookup(pcm_sep_type, codec_id)) == NULL) {
			errmsg = "SEP codec not supported";
			goto fail;
		}

		if (a2dp_configuration_size == 0)
			/* Default to capabilities supported by the local SEP. */
			memcpy(&a2dp_configuration, &sep->config.capabilities, sep->config.caps_size);
		else {
			/* Validate the size of provided configuration blob. */
			if (a2dp_configuration_size != remote_sep_cfg->caps_size) {
				errmsg = a2dp_check_strerror(A2DP_CHECK_ERR_SIZE);
				goto fail;
			}
		}

		/* Cap selected configuration with the remote SEP capabilities.
		 * This is required to prevent unsupported configuration from
		 * being set which will lead to A2DP disconnection. */
		sep->caps_helpers->intersect(&a2dp_configuration, &remote_sep_cfg->capabilities);

		if (channels != 0)
			sep->caps_helpers->select_channel_mode(&a2dp_configuration, pcm_sep_stream, channels);
		if (rate != 0)
			sep->caps_helpers->select_sample_rate(&a2dp_configuration, pcm_sep_stream, rate);

		if (a2dp_configuration_size == 0) {
			/* Setup default configuration if it was not provided. */
			if (a2dp_select_configuration(sep, &a2dp_configuration, sep->config.caps_size) == -1)
				goto fail;
		}
		else if (conformance_check) {
			enum a2dp_check_err rv;
			/* Validate provided configuration. */
			if ((rv = a2dp_check_configuration(sep, &a2dp_configuration,
						a2dp_configuration_size)) != A2DP_CHECK_OK) {
				errmsg = a2dp_check_strerror(rv);
				goto fail;
			}
		}

		if (ba_transport_select_codec_a2dp(t, remote_sep_cfg, &a2dp_configuration) == -1)
			goto fail;

	}
	else {

		uint8_t codec_id;
		if ((codec_id = hfp_codec_id_from_string(codec_name)) == HFP_CODEC_UNDEFINED) {
			errmsg = "HFP codec not available";
			goto fail;
		}

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
}

static void bluealsa_rfcomm_open(GDBusMethodInvocation *inv, void *userdata) {

	struct ba_rfcomm *r = userdata;
	int fds[2] = { -1, -1 };

	if (r->handler_fd != -1) {
		g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
				G_DBUS_ERROR_LIMITS_EXCEEDED, "%s", strerror(EBUSY));
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

static GVariant *bluealsa_pcm_get_property(const char *property,
		GError **error, void *userdata) {

	struct ba_transport_pcm *pcm = userdata;
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
	if (strcmp(property, "Running") == 0)
		return ba_variant_new_pcm_running(pcm);
	if (strcmp(property, "Format") == 0)
		return ba_variant_new_pcm_format(pcm);
	if (strcmp(property, "Channels") == 0)
		return ba_variant_new_pcm_channels(pcm);
	if (strcmp(property, "ChannelMap") == 0)
		return ba_variant_new_pcm_channel_map(pcm);
	if (strcmp(property, "Rate") == 0)
		return ba_variant_new_pcm_rate(pcm);
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
	if (strcmp(property, "ClientDelay") == 0)
		return ba_variant_new_pcm_client_delay(pcm);
	if (strcmp(property, "SoftVolume") == 0)
		return ba_variant_new_pcm_soft_volume(pcm);
	if (strcmp(property, "Volume") == 0)
		return ba_variant_new_pcm_volume(pcm);

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

	struct ba_transport_pcm *pcm = userdata;
	struct ba_transport *t = pcm->t;

	const bool is_sco = t->profile & BA_TRANSPORT_PROFILE_MASK_SCO;
	const int volume_max = is_sco ? HFP_VOLUME_GAIN_MAX : BLUEZ_A2DP_VOLUME_MAX;

	if (strcmp(property, "ClientDelay") == 0) {
		pcm->client_delay_dms = g_variant_get_int16(value);
		ba_transport_pcm_delay_sync(pcm, BA_DBUS_PCM_UPDATE_CLIENT_DELAY);
		return TRUE;
	}

	if (strcmp(property, "SoftVolume") == 0) {

		const bool soft_volume = g_variant_get_boolean(value);

		/* In case when the software volume was just enabled, set the volume level
		 * to the maximum. This will prevent volume change during the transition.
		 * In case of disabling the software volume, we will restore the hardware
		 * volume level, so the volume control will indicate the correct level. */
		const int volume = soft_volume ? volume_max : ba_transport_pcm_get_hardware_volume(pcm);
		const int level = ba_transport_pcm_volume_range_to_level(volume, volume_max);

		pthread_mutex_lock(&pcm->mutex);

		debug("Setting software volume: %s", soft_volume ? "true" : "false");
		pcm->soft_volume = soft_volume;

		for (size_t i = 0; i < pcm->channels; i++)
			ba_transport_pcm_volume_set(&pcm->volume[i], &level, NULL, NULL);

		pthread_mutex_unlock(&pcm->mutex);

		ba_transport_pcm_volume_sync(pcm, BA_DBUS_PCM_UPDATE_SOFT_VOLUME | BA_DBUS_PCM_UPDATE_VOLUME);
		return true;
	}

	if (strcmp(property, "Volume") == 0) {

		size_t channels = 0;
		const uint8_t *volume = g_variant_get_fixed_array(value, &channels, sizeof(uint8_t));

		if (channels != pcm->channels) {
			*error = g_error_new(G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
					"Invalid number of channels: %zu != %u", channels, pcm->channels);
			return false;
		}

		pthread_mutex_lock(&pcm->mutex);

		for (size_t i = 0; i < channels; i++) {

			const bool muted = !!(volume[i] & 0x80);
			const int level = ba_transport_pcm_volume_range_to_level(volume[i] & 0x7F, volume_max);

			debug("Setting volume [ch=%zu]: %u [%.2f dB] [%c]",
					i, volume[i] & 0x7F, 0.01 * level, muted ? 'M' : ' ');
			ba_transport_pcm_volume_set(&pcm->volume[i], &level, &muted, NULL);

		}

		pthread_mutex_unlock(&pcm->mutex);

		ba_transport_pcm_volume_sync(pcm, BA_DBUS_PCM_UPDATE_VOLUME);
		return true;
	}

	g_assert_not_reached();
	return false;
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
		.get_property = bluealsa_pcm_get_property,
		.set_property = bluealsa_pcm_set_property,
	};

	GDBusObjectSkeleton *skeleton = NULL;
	OrgBluealsaPcm1Skeleton *ifs_pcm = NULL;

	if ((skeleton = g_dbus_object_skeleton_new(pcm->ba_dbus_path)) == NULL)
		goto fail;

	if ((ifs_pcm = org_bluealsa_pcm1_skeleton_new(&vtable,
					pcm, (GDestroyNotify)ba_transport_pcm_unref)) == NULL)
		goto fail;

	g_dbus_interface_skeleton_set_flags(G_DBUS_INTERFACE_SKELETON(ifs_pcm),
			G_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_THREAD);

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

	if (mask & BA_DBUS_PCM_UPDATE_RUNNING)
		g_variant_builder_add(&props, "{sv}", "Running", ba_variant_new_pcm_running(pcm));
	if (mask & BA_DBUS_PCM_UPDATE_FORMAT)
		g_variant_builder_add(&props, "{sv}", "Format", ba_variant_new_pcm_format(pcm));
	if (mask & BA_DBUS_PCM_UPDATE_CHANNELS)
		g_variant_builder_add(&props, "{sv}", "Channels", ba_variant_new_pcm_channels(pcm));
	if (mask & BA_DBUS_PCM_UPDATE_CHANNEL_MAP)
		g_variant_builder_add(&props, "{sv}", "ChannelMap", ba_variant_new_pcm_channel_map(pcm));
	if (mask & BA_DBUS_PCM_UPDATE_RATE)
		g_variant_builder_add(&props, "{sv}", "Rate", ba_variant_new_pcm_rate(pcm));
	if (mask & BA_DBUS_PCM_UPDATE_CODEC)
		g_variant_builder_add(&props, "{sv}", "Codec", ba_variant_new_pcm_codec(pcm));
	if (mask & BA_DBUS_PCM_UPDATE_CODEC_CONFIG)
		g_variant_builder_add(&props, "{sv}", "CodecConfiguration", ba_variant_new_pcm_codec_config(pcm));
	if (mask & BA_DBUS_PCM_UPDATE_DELAY)
		g_variant_builder_add(&props, "{sv}", "Delay", ba_variant_new_pcm_delay(pcm));
	if (mask & BA_DBUS_PCM_UPDATE_CLIENT_DELAY)
		g_variant_builder_add(&props, "{sv}", "ClientDelay", ba_variant_new_pcm_client_delay(pcm));
	if (mask & BA_DBUS_PCM_UPDATE_SOFT_VOLUME)
		g_variant_builder_add(&props, "{sv}", "SoftVolume", ba_variant_new_pcm_soft_volume(pcm));
	if (mask & BA_DBUS_PCM_UPDATE_VOLUME)
		g_variant_builder_add(&props, "{sv}", "Volume", ba_variant_new_pcm_volume(pcm));

	g_dbus_connection_emit_properties_changed(config.dbus, pcm->ba_dbus_path,
			BLUEALSA_IFACE_PCM, g_variant_builder_end(&props), NULL, NULL);

}

void bluealsa_dbus_pcm_unregister(struct ba_transport_pcm *pcm) {

	if (!pcm->ba_dbus_exported)
		return;

	g_dbus_object_manager_server_unexport(bluealsa_dbus_manager, pcm->ba_dbus_path);
	pcm->ba_dbus_exported = false;

}

static GVariant *bluealsa_rfcomm_get_property(const char *property,
		GError **error, void *userdata) {
	(void)error;

	struct ba_rfcomm *r = userdata;
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
		.get_property = bluealsa_rfcomm_get_property,
	};

	GDBusObjectSkeleton *skeleton = NULL;
	OrgBluealsaRfcomm1Skeleton *ifs_rfcomm = NULL;

	if ((skeleton = g_dbus_object_skeleton_new(r->ba_dbus_path)) == NULL)
		goto fail;

	if ((ifs_rfcomm = org_bluealsa_rfcomm1_skeleton_new(&vtable,
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

	g_dbus_connection_emit_properties_changed(config.dbus, r->ba_dbus_path,
			BLUEALSA_IFACE_RFCOMM, g_variant_builder_end(&props), NULL, NULL);

}

void bluealsa_dbus_rfcomm_unregister(struct ba_rfcomm *r) {
	if (!r->ba_dbus_exported)
		return;
	g_dbus_object_manager_server_unexport(bluealsa_dbus_manager, r->ba_dbus_path);
	r->ba_dbus_exported = false;
}
