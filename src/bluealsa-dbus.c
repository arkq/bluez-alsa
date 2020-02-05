/*
 * BlueALSA - bluealsa-dbus.c
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "bluealsa-dbus.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <bluetooth/hci.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib-object.h>
#include <glib.h>

#include "ba-adapter.h"
#include "ba-device.h"
#include "bluealsa-iface.h"
#include "bluealsa.h"
#include "hfp.h"
#include "shared/defs.h"
#include "shared/log.h"

static GVariant *ba_variant_new_device_path(const struct ba_device *d) {
	return g_variant_new_object_path(d->bluez_dbus_path);
}

static GVariant *ba_variant_new_device_battery(const struct ba_device *d) {
	return g_variant_new_byte(d->battery_level);
}

static GVariant *ba_variant_new_rfcomm_features(const struct ba_transport *t) {
	return g_variant_new_uint32(t->rfcomm.hfp_features);
}

static GVariant *ba_variant_new_modes(const struct ba_transport *t) {
	static const char *modes[] = {
		BLUEALSA_PCM_MODE_SOURCE, BLUEALSA_PCM_MODE_SINK };
	if (t->type.profile & BA_TRANSPORT_PROFILE_A2DP_SOURCE)
		return g_variant_new_strv(&modes[1], 1);
	if (t->type.profile & BA_TRANSPORT_PROFILE_A2DP_SINK)
		return g_variant_new_strv(&modes[0], 1);
	if (IS_BA_TRANSPORT_PROFILE_SCO(t->type.profile))
		return g_variant_new_strv(modes, 2);
	return g_variant_new_strv(modes, 0);
}

static GVariant *ba_variant_new_format(const struct ba_transport *t) {
	if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_A2DP)
		return g_variant_new_uint16(t->a2dp.pcm.format);
	if (IS_BA_TRANSPORT_PROFILE_SCO(t->type.profile))
		return g_variant_new_uint16(t->sco.spk_pcm.format);
	return g_variant_new_uint16(0);
}

static GVariant *ba_variant_new_channels(const struct ba_transport *t) {
	if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_A2DP)
		return g_variant_new_byte(t->a2dp.pcm.channels);
	if (IS_BA_TRANSPORT_PROFILE_SCO(t->type.profile))
		return g_variant_new_byte(t->sco.spk_pcm.channels);
	return g_variant_new_byte(0);
}

static GVariant *ba_variant_new_sampling(const struct ba_transport *t) {
	if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_A2DP)
		return g_variant_new_uint32(t->a2dp.pcm.sampling);
	if (IS_BA_TRANSPORT_PROFILE_SCO(t->type.profile))
		return g_variant_new_uint32(t->sco.spk_pcm.sampling);
	return g_variant_new_uint32(0);
}

static GVariant *ba_variant_new_codec(const struct ba_transport *t) {
	return g_variant_new_uint16(t->type.codec);
}

static GVariant *ba_variant_new_delay(const struct ba_transport *t) {
	return g_variant_new_uint16(ba_transport_get_delay(t));
}

static GVariant *ba_variant_new_volume(const struct ba_transport *t) {
	return g_variant_new_uint16(ba_transport_get_volume_packed(t));
}

static void bluealsa_manager_get_pcms(GDBusMethodInvocation *inv, void *userdata) {
	(void)userdata;

	GVariantBuilder pcms;
	g_variant_builder_init(&pcms, G_VARIANT_TYPE("a{oa{sv}}"));

	struct ba_adapter *a;
	size_t i;

	for (i = 0; i < HCI_MAX_DEV; i++) {

		if ((a = ba_adapter_lookup(i)) == NULL)
			continue;

		GHashTableIter iter_d, iter_t;
		struct ba_device *d;
		struct ba_transport *t;

		pthread_mutex_lock(&a->devices_mutex);
		g_hash_table_iter_init(&iter_d, a->devices);
		while (g_hash_table_iter_next(&iter_d, NULL, (gpointer)&d)) {

			pthread_mutex_lock(&d->transports_mutex);
			g_hash_table_iter_init(&iter_t, d->transports);
			while (g_hash_table_iter_next(&iter_t, NULL, (gpointer)&t)) {

				if (t->type.profile & BA_TRANSPORT_PROFILE_RFCOMM)
					continue;

				GVariantBuilder props;
				g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
				g_variant_builder_add(&props, "{sv}", "Device", ba_variant_new_device_path(t->d));
				g_variant_builder_add(&props, "{sv}", "Modes", ba_variant_new_modes(t));
				g_variant_builder_add(&props, "{sv}", "Format", ba_variant_new_format(t));
				g_variant_builder_add(&props, "{sv}", "Channels", ba_variant_new_channels(t));
				g_variant_builder_add(&props, "{sv}", "Sampling", ba_variant_new_sampling(t));
				g_variant_builder_add(&props, "{sv}", "Codec", ba_variant_new_codec(t));
				g_variant_builder_add(&props, "{sv}", "Delay", ba_variant_new_delay(t));
				g_variant_builder_add(&props, "{sv}", "Volume", ba_variant_new_volume(t));

				g_variant_builder_add(&pcms, "{oa{sv}}", t->ba_dbus_path, &props);
				g_variant_builder_clear(&props);
			}

			pthread_mutex_unlock(&d->transports_mutex);
		}

		pthread_mutex_unlock(&a->devices_mutex);
		ba_adapter_unref(a);

	}

	g_dbus_method_invocation_return_value(inv, g_variant_new("(a{oa{sv}})", &pcms));
	g_variant_builder_clear(&pcms);
}

static void bluealsa_manager_method_call(GDBusConnection *conn, const char *sender,
		const char *path, const char *interface, const char *method, GVariant *params,
		GDBusMethodInvocation *invocation, void *userdata) {
	debug("Called: %s.%s()", interface, method);
	(void)conn;
	(void)sender;
	(void)path;
	(void)params;

	if (strcmp(method, "GetPCMs") == 0)
		bluealsa_manager_get_pcms(invocation, userdata);

}

/**
 * Register BlueALSA D-Bus manager interface. */
unsigned int bluealsa_dbus_manager_register(GError **error) {
	static const GDBusInterfaceVTable vtable = {
		.method_call = bluealsa_manager_method_call };
	return g_dbus_connection_register_object(config.dbus, "/org/bluealsa",
			(GDBusInterfaceInfo *)&bluealsa_iface_manager, &vtable, NULL, NULL, error);
}

static gboolean bluealsa_pcm_controller(GIOChannel *ch, GIOCondition condition,
		void *userdata) {
	(void)condition;

	struct ba_transport_pcm *pcm = (struct ba_transport_pcm *)userdata;
	struct ba_transport *t = pcm->t;
	char command[32];
	size_t len;

	switch (g_io_channel_read_chars(ch, command, sizeof(command), &len, NULL)) {
	case G_IO_STATUS_ERROR:
		error("Couldn't read controller channel");
		return TRUE;
	case G_IO_STATUS_NORMAL:
		if (strncmp(command, BLUEALSA_PCM_CTRL_DRAIN, len) == 0) {
			if (pcm->mode == BA_TRANSPORT_PCM_MODE_SINK)
				ba_transport_drain_pcm(t);
			g_io_channel_write_chars(ch, "OK", -1, &len, NULL);
		}
		else if (strncmp(command, BLUEALSA_PCM_CTRL_DROP, len) == 0) {
			if (pcm->mode == BA_TRANSPORT_PCM_MODE_SINK)
				ba_transport_send_signal(t, BA_TRANSPORT_SIGNAL_PCM_DROP);
			g_io_channel_write_chars(ch, "OK", -1, &len, NULL);
		}
		else if (strncmp(command, BLUEALSA_PCM_CTRL_PAUSE, len) == 0) {
			ba_transport_set_state(t, BA_TRANSPORT_STATE_PAUSED);
			ba_transport_send_signal(t, BA_TRANSPORT_SIGNAL_PCM_PAUSE);
			g_io_channel_write_chars(ch, "OK", -1, &len, NULL);
		}
		else if (strncmp(command, BLUEALSA_PCM_CTRL_RESUME, len) == 0) {
			ba_transport_set_state(t, BA_TRANSPORT_STATE_ACTIVE);
			ba_transport_send_signal(t, BA_TRANSPORT_SIGNAL_PCM_RESUME);
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
		ba_transport_release_pcm(pcm);
		ba_transport_send_signal(t, BA_TRANSPORT_SIGNAL_PCM_CLOSE);
		/* remove channel from watch */
		return FALSE;
	}

	return TRUE;
}

static void bluealsa_pcm_open(GDBusMethodInvocation *inv, void *userdata) {

	GVariant *params = g_dbus_method_invocation_get_parameters(inv);
	struct ba_transport *t = (struct ba_transport *)userdata;
	int pcm_fds[4] = { -1, -1, -1, -1 };
	bool locked = false;
	size_t i;

	const char *mode;
	g_variant_get(params, "(&s)", &mode);

	bool is_sink = strcmp(mode, BLUEALSA_PCM_MODE_SINK) == 0;
	if (!is_sink && strcmp(mode, BLUEALSA_PCM_MODE_SOURCE) != 0) {
		g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
				G_DBUS_ERROR_INVALID_ARGS, "Invalid operation mode: %s", mode);
		goto fail;
	}

	struct ba_transport_pcm *pcm = NULL;
	if ((is_sink && t->type.profile & BA_TRANSPORT_PROFILE_A2DP_SOURCE) ||
			(!is_sink && t->type.profile & BA_TRANSPORT_PROFILE_A2DP_SINK))
		pcm = &t->a2dp.pcm;
	else if (IS_BA_TRANSPORT_PROFILE_SCO(t->type.profile)) {

		if (is_sink)
			pcm = &t->sco.spk_pcm;
		else
			pcm = &t->sco.mic_pcm;

		/* preliminary check whether HFP codes is selected */
		if (t->type.codec == HFP_CODEC_UNDEFINED) {
			g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
					G_DBUS_ERROR_FAILED, "HFP audio codec not selected");
			goto fail;
		}

	}

	if (pcm == NULL) {
		g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
				G_DBUS_ERROR_NOT_SUPPORTED, "Operation mode not supported");
		goto fail;
	}

	/* We must ensure that transport release is not in progress before
	 * accessing transport critical section. Otherwise, we might have
	 * the IO thread close it in the middle of open procedure! */
	ba_transport_pthread_cleanup_lock(t);
	locked = true;

	if (pcm->fd != -1) {
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

	/* get correct PIPE endpoint - PIPE is unidirectional */
	pcm->fd = pcm_fds[is_sink ? 0 : 1];

	/* set our internal endpoint as non-blocking. */
	if (fcntl(pcm->fd, F_SETFL, O_NONBLOCK) == -1) {
		g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
				G_DBUS_ERROR_FAILED, "Setup PIPE: %s", strerror(errno));
		goto fail;
	}

	/* notify our IO thread that the FIFO has been created */
	ba_transport_send_signal(t, BA_TRANSPORT_SIGNAL_PCM_OPEN);

	/* A2DP source profile should be initialized (acquired) only if the audio
	 * is about to be transferred. It is most likely, that BT headset will not
	 * run voltage converter (power-on its circuit board) until the transport
	 * is acquired - in order to extend battery life. */
	if (t->type.profile == BA_TRANSPORT_PROFILE_A2DP_SOURCE)
		if (t->acquire(t) == -1) {
			g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
					G_DBUS_ERROR_FAILED, "Acquire transport: %s", strerror(errno));
			goto fail;
		}

	ba_transport_pthread_cleanup_unlock(t);

	GIOChannel *ch = g_io_channel_unix_new(pcm_fds[2]);
	g_io_add_watch_full(ch, G_PRIORITY_DEFAULT, G_IO_IN,
			bluealsa_pcm_controller, pcm, NULL);
	g_io_channel_set_close_on_unref(ch, TRUE);
	g_io_channel_set_encoding(ch, NULL, NULL);
	g_io_channel_unref(ch);

	int fds[2] = { pcm_fds[is_sink ? 1 : 0], pcm_fds[3] };
	GUnixFDList *fd_list = g_unix_fd_list_new_from_array(fds, 2);
	g_dbus_method_invocation_return_value_with_unix_fd_list(inv,
			g_variant_new("(hh)", 0, 1), fd_list);
	g_object_unref(fd_list);

	return;

fail:
	if (locked)
		ba_transport_pthread_cleanup_unlock(t);
	/* clean up created file descriptors */
	for (i = 0; i < ARRAYSIZE(pcm_fds); i++)
		if (pcm_fds[i] != -1)
			close(pcm_fds[i]);
}

static void bluealsa_pcm_select_codec(GDBusMethodInvocation *inv, void *userdata) {

	GVariant *params = g_dbus_method_invocation_get_parameters(inv);
	struct ba_transport *t = (struct ba_transport *)userdata;
	GVariantIter *properties;
	GVariant *value = NULL;
	const char *property;
	uint16_t codec;

	g_variant_get(params, "(qa{sv})", &codec, &properties);
	while (g_variant_iter_next(properties, "{&sv}", &property, &value)) {
		g_variant_unref(value);
		value = NULL;
	}

	/* Lock transport before codec selection, so we will have
	 * an exclusive access to the transport critical section. */
	ba_transport_pthread_cleanup_lock(t);

	if (ba_transport_select_codec(t, codec) == -1) {
		error("Couldn't select codec: %#x: %s", codec, strerror(errno));
		goto fail;
	}

	g_dbus_method_invocation_return_value(inv, NULL);
	goto final;

fail:
	g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
			G_DBUS_ERROR_FAILED, "%s", strerror(errno));

final:
	ba_transport_pthread_cleanup_unlock(t);
	g_variant_iter_free(properties);
	if (value != NULL)
		g_variant_unref(value);
}

static void bluealsa_pcm_method_call(GDBusConnection *conn, const char *sender,
		const char *path, const char *interface, const char *method, GVariant *params,
		GDBusMethodInvocation *invocation, void *userdata) {
	debug("Called: %s.%s()", interface, method);
	(void)conn;
	(void)sender;
	(void)path;
	(void)params;

	if (strcmp(method, "Open") == 0)
		bluealsa_pcm_open(invocation, userdata);
	else if (strcmp(method, "SelectCodec") == 0)
		bluealsa_pcm_select_codec(invocation, userdata);

}

static void bluealsa_rfcomm_open(GDBusMethodInvocation *inv, void *userdata) {

	struct ba_transport *t = (struct ba_transport *)userdata;
	int fds[2] = { -1, -1 };

	if (t->rfcomm.handler_fd != -1) {
		g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
				G_DBUS_ERROR_FAILED, "%s", strerror(EBUSY));
		return;
	}

	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, fds) == -1) {
		g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
				G_DBUS_ERROR_FAILED, "Create socket: %s", strerror(errno));
		return;
	}

	t->rfcomm.handler_fd = fds[0];
	ba_transport_send_signal(t, BA_TRANSPORT_SIGNAL_PING);

	GUnixFDList *fd_list = g_unix_fd_list_new_from_array(&fds[1], 1);
	g_dbus_method_invocation_return_value_with_unix_fd_list(inv,
			g_variant_new("(h)", 0), fd_list);
	g_object_unref(fd_list);
}

static void bluealsa_rfcomm_method_call(GDBusConnection *conn, const char *sender,
		const char *path, const char *interface, const char *method, GVariant *params,
		GDBusMethodInvocation *invocation, void *userdata) {
	debug("Called: %s.%s()", interface, method);
	(void)conn;
	(void)sender;
	(void)path;
	(void)params;

	if (strcmp(method, "Open") == 0)
		bluealsa_rfcomm_open(invocation, userdata);

}

static GVariant *bluealsa_pcm_get_property(GDBusConnection *conn,
		const char *sender, const char *path, const char *interface,
		const char *property, GError **error, void *userdata) {
	(void)conn;
	(void)sender;
	(void)path;
	(void)interface;

	struct ba_transport *t = (struct ba_transport *)userdata;
	struct ba_device *d = t->d;

	if (strcmp(property, "Device") == 0)
		return ba_variant_new_device_path(d);
	if (strcmp(property, "Modes") == 0)
		return ba_variant_new_modes(t);
	if (strcmp(property, "Format") == 0)
		return ba_variant_new_format(t);
	if (strcmp(property, "Channels") == 0)
		return ba_variant_new_channels(t);
	if (strcmp(property, "Sampling") == 0)
		return ba_variant_new_sampling(t);
	if (strcmp(property, "Codec") == 0)
		return ba_variant_new_codec(t);
	if (strcmp(property, "Delay") == 0)
		return ba_variant_new_delay(t);
	if (strcmp(property, "Volume") == 0)
		return ba_variant_new_volume(t);

	*error = g_error_new(G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED,
			"Property not supported '%s'", property);
	return NULL;
}

static GVariant *bluealsa_rfcomm_get_property(GDBusConnection *conn,
		const char *sender, const char *path, const char *interface,
		const char *property, GError **error, void *userdata) {
	(void)conn;
	(void)sender;
	(void)path;
	(void)interface;

	struct ba_transport *t = (struct ba_transport *)userdata;
	struct ba_device *d = t->d;

	if (strcmp(property, "Mode") == 0) {
		if (t->type.profile & BA_TRANSPORT_PROFILE_HFP_AG)
			return g_variant_new_string(BLUEALSA_RFCOMM_MODE_HFP_AG);
		if (t->type.profile & BA_TRANSPORT_PROFILE_HFP_HF)
			return g_variant_new_string(BLUEALSA_RFCOMM_MODE_HFP_HF);
		if (t->type.profile & BA_TRANSPORT_PROFILE_HSP_AG)
			return g_variant_new_string(BLUEALSA_RFCOMM_MODE_HSP_AG);
		if (t->type.profile & BA_TRANSPORT_PROFILE_HSP_HS)
			return g_variant_new_string(BLUEALSA_RFCOMM_MODE_HSP_HS);
	}
	if (strcmp(property, "Features") == 0)
		return ba_variant_new_rfcomm_features(t);
	if (strcmp(property, "Battery") == 0)
		return ba_variant_new_device_battery(d);

	*error = g_error_new(G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED,
			"Property not supported '%s'", property);
	return NULL;
}

static gboolean bluealsa_pcm_set_property(GDBusConnection *conn,
		const gchar *sender, const gchar *path, const gchar *interface,
		const gchar *property, GVariant *value, GError **error, gpointer userdata) {
	(void)conn;
	(void)sender;
	(void)path;
	(void)interface;

	struct ba_transport *t = (struct ba_transport *)userdata;

	if (strcmp(property, "Volume") == 0) {
		ba_transport_set_volume_packed(t, g_variant_get_uint16(value));
		return TRUE;
	}

	*error = g_error_new(G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED,
			"Property not supported '%s'", property);
	return FALSE;
}

/**
 * Register BlueALSA D-Bus PCM interface. */
unsigned int bluealsa_dbus_pcm_register(struct ba_transport *t, GError **error) {

	static const GDBusInterfaceVTable vtable = {
		.method_call = bluealsa_pcm_method_call,
		.get_property = bluealsa_pcm_get_property,
		.set_property = bluealsa_pcm_set_property,
	};

	if ((t->ba_dbus_id = g_dbus_connection_register_object(config.dbus,
					t->ba_dbus_path, (GDBusInterfaceInfo *)&bluealsa_iface_pcm,
					&vtable, t, NULL, error)) != 0) {

		ba_transport_ref(t);

		GVariantBuilder props;
		g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
		g_variant_builder_add(&props, "{sv}", "Device", ba_variant_new_device_path(t->d));
		g_variant_builder_add(&props, "{sv}", "Modes", ba_variant_new_modes(t));
		g_variant_builder_add(&props, "{sv}", "Format", ba_variant_new_format(t));
		g_variant_builder_add(&props, "{sv}", "Channels", ba_variant_new_channels(t));
		g_variant_builder_add(&props, "{sv}", "Sampling", ba_variant_new_sampling(t));
		g_variant_builder_add(&props, "{sv}", "Codec", ba_variant_new_codec(t));
		g_variant_builder_add(&props, "{sv}", "Delay", ba_variant_new_delay(t));
		g_variant_builder_add(&props, "{sv}", "Volume", ba_variant_new_volume(t));

		g_dbus_connection_emit_signal(config.dbus, NULL,
				"/org/bluealsa", BLUEALSA_IFACE_MANAGER, "PCMAdded",
				g_variant_new("(oa{sv})", t->ba_dbus_path, &props), NULL);
		g_variant_builder_clear(&props);

	}

	return t->ba_dbus_id;
}

void bluealsa_dbus_pcm_update(struct ba_transport *t, unsigned int mask) {

	GVariantBuilder props;
	g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));

	if (mask & BA_DBUS_PCM_UPDATE_FORMAT)
		g_variant_builder_add(&props, "{sv}", "Format", ba_variant_new_format(t));
	if (mask & BA_DBUS_PCM_UPDATE_CHANNELS)
		g_variant_builder_add(&props, "{sv}", "Channels", ba_variant_new_channels(t));
	if (mask & BA_DBUS_PCM_UPDATE_SAMPLING)
		g_variant_builder_add(&props, "{sv}", "Sampling", ba_variant_new_sampling(t));
	if (mask & BA_DBUS_PCM_UPDATE_CODEC)
		g_variant_builder_add(&props, "{sv}", "Codec", ba_variant_new_codec(t));
	if (mask & BA_DBUS_PCM_UPDATE_DELAY)
		g_variant_builder_add(&props, "{sv}", "Delay", ba_variant_new_delay(t));
	if (mask & BA_DBUS_PCM_UPDATE_VOLUME)
		g_variant_builder_add(&props, "{sv}", "Volume", ba_variant_new_volume(t));

	g_dbus_connection_emit_signal(config.dbus, NULL, t->ba_dbus_path,
			"org.freedesktop.DBus.Properties", "PropertiesChanged",
			g_variant_new("(sa{sv}as)", BLUEALSA_IFACE_PCM, &props, NULL), NULL);

	g_variant_builder_clear(&props);
}

void bluealsa_dbus_pcm_unregister(struct ba_transport *t) {

	if (t->ba_dbus_id == 0)
		return;

	g_dbus_connection_unregister_object(config.dbus, t->ba_dbus_id);
	t->ba_dbus_id = 0;

	g_dbus_connection_emit_signal(config.dbus, NULL,
			"/org/bluealsa", BLUEALSA_IFACE_MANAGER, "PCMRemoved",
			g_variant_new("(o)", t->ba_dbus_path), NULL);

	ba_transport_unref(t);
}

/**
 * Register BlueALSA D-Bus RFCOMM interface. */
unsigned int bluealsa_dbus_rfcomm_register(struct ba_transport *t, GError **error) {

	static const GDBusInterfaceVTable vtable = {
		.method_call = bluealsa_rfcomm_method_call,
		.get_property = bluealsa_rfcomm_get_property,
	};

	if ((t->ba_dbus_id = g_dbus_connection_register_object(config.dbus,
					t->ba_dbus_path, (GDBusInterfaceInfo *)&bluealsa_iface_rfcomm,
					&vtable, t, NULL, error)) != 0)
		ba_transport_ref(t);

	return t->ba_dbus_id;
}

void bluealsa_dbus_rfcomm_update(struct ba_transport *t, unsigned int mask) {

	GVariantBuilder props;
	g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));

	if (mask & BA_DBUS_RFCOMM_UPDATE_FEATURES)
		g_variant_builder_add(&props, "{sv}", "Features", ba_variant_new_rfcomm_features(t));
	if (mask & BA_DBUS_RFCOMM_UPDATE_BATTERY)
		g_variant_builder_add(&props, "{sv}", "Battery", ba_variant_new_device_battery(t->d));

	g_dbus_connection_emit_signal(config.dbus, NULL, t->ba_dbus_path,
			"org.freedesktop.DBus.Properties", "PropertiesChanged",
			g_variant_new("(sa{sv}as)", BLUEALSA_IFACE_RFCOMM, &props, NULL), NULL);

	g_variant_builder_clear(&props);
}

void bluealsa_dbus_rfcomm_unregister(struct ba_transport *t) {

	if (t->ba_dbus_id == 0)
		return;

	g_dbus_connection_unregister_object(config.dbus, t->ba_dbus_id);
	t->ba_dbus_id = 0;

	ba_transport_unref(t);
}
