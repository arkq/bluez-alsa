/*
 * BlueALSA - ofono.c
 * Copyright (c) 2016-2018 Arkadiusz Bokowy
 *               2018 Thierry Bultel
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 * When oFono is running on a system, it registers itself to Bluez as an HFP
 * profile, which conflicts with our internal "--hfp-ag" and "--hpf-hf" ones.
 * This file is an implementation of the oFono backed for bluez-alsa.
 *
 * For more details, see:
 * https://github.com/rilmodem/ofono/blob/master/doc/handsfree-audio-api.txt
 *
 */

#include "ofono.h"

#include <errno.h>

#include <gio/gunixfdlist.h>

#include "bluealsa.h"
#include "ctl.h"
#include "ofono-iface.h"
#include "transport.h"
#include "shared/log.h"

#define OFONO_FAKE_DEV_ID 0xffff

static unsigned int dbus_agent_object_id = 0;

/**
 * Authorize oFono SCO connection.
 *
 * @param fd SCO socket file descriptor.
 * @return On success this function returns 0. Otherwise, -1 is returned and
 *	 errno is set to indicate the error. */
static int ofono_sco_socket_authorize(int fd) {

	struct pollfd pfd = { fd, POLLOUT, 0 };
	char c;

	if (poll(&pfd, 1, 0) == -1)
		return -1;

	/* If socket is not writable, it means that it is in the defer setup
	 * state, so it needs to be read to authorize the connection. */
	if (!(pfd.revents & POLLOUT) && read(fd, &c, 1) == -1)
		return -1;

	return 0;
}

/**
 * Ask oFono to connect to a card (in return it will call NewConnection). */
static int ofono_acquire_bt_sco(struct ba_transport *t) {

	GDBusConnection *conn = config.dbus;
	GDBusMessage *msg = NULL, *rep = NULL;
	GError *err = NULL;
	int ret = 0;

	debug("Requesting new oFono SCO connection: %s", t->device->name);

	msg = g_dbus_message_new_method_call(OFONO_SERVICE, t->device->name,
			OFONO_IFACE_HF_AUDIO_CARD, "Connect");

	if ((rep = g_dbus_connection_send_message_with_reply_sync(conn, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, &err)) == NULL)
		goto fail;

	if (g_dbus_message_get_message_type(rep) == G_DBUS_MESSAGE_TYPE_ERROR) {
		g_dbus_message_to_gerror(rep, &err);
		goto fail;
	}

	goto final;

fail:
	ret = -1;

final:
	if (msg != NULL)
		g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);
	if (err != NULL) {
		warn("Couldn't connect to card: %s", err->message);
		g_error_free(err);
	}

	return ret;
}

/**
 * Disconnects from a card.
 *
 * This is basically called when no PCM client is connected, in order to save
 * bandwidth for Bluetooth.
 *
 * @return On success this function returns 0. Otherwise -1 is returned. */
static int ofono_release_bt_sco(struct ba_transport *t) {

	if (t->bt_fd == -1)
		return 0;

	debug("Closing oFono SCO: %d", t->bt_fd);

	shutdown(t->bt_fd, SHUT_RDWR);
	close(t->bt_fd);

	t->bt_fd = -1;
	t->codec = HFP_CODEC_UNDEFINED;

	bluealsa_ctl_send_event(BA_EVENT_TRANSPORT_CHANGED, &t->device->addr,
			BA_PCM_TYPE_SCO | BA_PCM_STREAM_PLAYBACK | BA_PCM_STREAM_CAPTURE);

	return 0;
}

/**
 * Create new oFono transport.
 *
 * It will be created with an unset codec, which is the condition for it
 * to be hidden to clients. The codec will be set when the phone call starts.
 *
 * @return On success, the pointer to the newly allocated transport structure
 *   is returned. If error occurs, NULL is returned and the errno variable is
 *   set to indicated the cause of the error. */
static struct ba_transport *ofono_transport_new(
		struct ba_device *device,
		const char *dbus_owner,
		const char *dbus_path,
		enum bluetooth_profile profile) {

	struct ba_transport *t;

	if ((t = transport_new_sco(device, dbus_owner, dbus_path,
					profile, HFP_CODEC_UNDEFINED)) == NULL)
		return NULL;

	t->sco.ofono = true;
	t->acquire = ofono_acquire_bt_sco;
	t->release = ofono_release_bt_sco;

	return t;
}

/**
 * Add new oFono card (phone). */
static void ofono_card_add(const char *dbus_sender, const char *card,
		GVariantIter *properties) {

	const char *key = NULL;
	GVariant *value = NULL;
	gchar *path = g_strdup_printf("/ofono%s", card);
	enum bluetooth_profile profile = BLUETOOTH_PROFILE_HFP_HF;
	bool devpool_mutex_locked = false;
	struct ba_transport *t;
	struct ba_device *d;
	bdaddr_t addr;

	while (g_variant_iter_next(properties, "{&sv}", &key, &value)) {
		if (strcmp(key, "RemoteAddress") == 0)
			str2ba(g_variant_get_string(value, NULL), &addr);
		else if (strcmp(key, "Type") == 0) {
			const char *type = g_variant_get_string(value, NULL);
			if (strcmp(type, OFONO_AUDIO_CARD_TYPE_AG) == 0)
				profile = BLUETOOTH_PROFILE_HFP_AG;
			else if (strcmp(type, OFONO_AUDIO_CARD_TYPE_HF) == 0)
				profile = BLUETOOTH_PROFILE_HFP_HF;
			else {
				error("Unsupported profile type: %s", type);
				goto fail;
			}
		}
		g_variant_unref(value);
		value = NULL;
	}

	debug("Adding new oFono card: %s", card);

	bluealsa_devpool_mutex_lock();
	devpool_mutex_locked = true;

	char name[sizeof(d->name)];
	ba2str(&addr, name);

	if ((d = device_new(OFONO_FAKE_DEV_ID, &addr, name)) == NULL) {
		error("Couldn't create device: %s", strerror(errno));
		goto fail;
	}

	bluealsa_device_insert(card, d);

	if ((t = ofono_transport_new(d, dbus_sender, path, profile)) == NULL) {
		error("Couldn't create new transport: %s", strerror(errno));
		goto fail;
	}

	transport_set_state(t, TRANSPORT_ACTIVE);

fail:
	if (devpool_mutex_locked)
		bluealsa_devpool_mutex_unlock();
	if (value != NULL)
		g_variant_unref(value);
	g_free(path);
}

/**
 * Get all oFono cards (phones). */
static int ofono_get_all_cards(void) {

	GDBusConnection *conn = config.dbus;
	GDBusMessage *msg = NULL, *rep = NULL;
	GError *err = NULL;
	int ret = 0;

	msg = g_dbus_message_new_method_call(OFONO_SERVICE, "/",
			OFONO_IFACE_HF_AUDIO_MANAGER, "GetCards");

	if ((rep = g_dbus_connection_send_message_with_reply_sync(conn, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, &err)) == NULL)
		goto fail;

	if (g_dbus_message_get_message_type(rep) == G_DBUS_MESSAGE_TYPE_ERROR) {
		g_dbus_message_to_gerror(rep, &err);
		goto fail;
	}

	const gchar *sender = g_dbus_message_get_sender(rep);
	GVariant *body = g_dbus_message_get_body(rep);

	GVariantIter *cards;
	GVariantIter *properties = NULL;
	const char *card;

	g_variant_get(body, "(a(oa{sv}))", &cards);
	while (g_variant_iter_next(cards, "(&oa{sv})", &card, &properties))
		ofono_card_add(sender, card, properties);

	goto final;

fail:
	ret = -1;

final:
	if (msg != NULL)
		g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);
	if (err != NULL) {
		warn("Couldn't get oFono cards: %s", err->message);
		g_error_free(err);
	}

	return ret;
}

/**
 * Remove all oFono cards and deletes associated transports. */
static void ofono_remove_all_cards(void) {

	GHashTableIter iter;
	struct ba_device *d;

	bluealsa_devpool_mutex_lock();

	g_hash_table_iter_init(&iter, config.devices);
	while (g_hash_table_iter_next(&iter, NULL, (gpointer)&d)) {
		if (d->hci_dev_id != OFONO_FAKE_DEV_ID)
			continue;
		g_hash_table_iter_remove(&iter);
	}

	bluealsa_devpool_mutex_unlock();
}

static void ofono_agent_new_connection(GDBusMethodInvocation *inv, void *userdata) {
	(void)userdata;

	GDBusMessage *msg = g_dbus_method_invocation_get_message(inv);
	GVariant *params = g_dbus_method_invocation_get_parameters(inv);
	bool devpool_mutex_locked = false;
	struct ba_transport *t;
	GError *err = NULL;
	gchar *path = NULL;
	GUnixFDList *fd_list;
	const char *card;
	uint8_t codec;
	int fd;

	g_variant_get(params, "(&ohy)", &card, &fd, &codec);

	fd_list = g_dbus_message_get_unix_fd_list(msg);
	if ((fd = g_unix_fd_list_get(fd_list, 0, &err)) == -1) {
		error("Couldn't obtain SCO socket: %s", err->message);
		goto fail;
	}

	bluealsa_devpool_mutex_lock();
	devpool_mutex_locked = true;

	path = g_strdup_printf("/ofono%s", card);
	if ((t = transport_lookup(config.devices, path)) == NULL) {
		error("Transport not available: %s", path);
		goto fail;
	}

	if (ofono_sco_socket_authorize(fd) == -1) {
		error("Couldn't authorize SCO connection: %s", strerror(errno));
		goto fail;
	}

	debug("New oFono SCO connection (codec: %#x): %d", codec, fd);

	t->bt_fd = fd;
	t->codec = codec;

	t->mtu_read = 48;
	t->mtu_write = 48;

	bluealsa_ctl_send_event(BA_EVENT_TRANSPORT_CHANGED, &t->device->addr,
			BA_PCM_TYPE_SCO | BA_PCM_STREAM_PLAYBACK | BA_PCM_STREAM_CAPTURE);
	transport_send_signal(t, TRANSPORT_BT_OPEN);

	g_dbus_method_invocation_return_value(inv, NULL);
	goto final;

fail:
	g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
		G_DBUS_ERROR_INVALID_ARGS, "Unable to get connection");

final:
	if (devpool_mutex_locked)
		bluealsa_devpool_mutex_unlock();
	if (path != NULL)
		g_free(path);
	if (err != NULL)
		g_error_free(err);
}

/**
 * Callback for the Release method, called when oFono is properly shutdown. */
static void ofono_agent_release(GDBusMethodInvocation *inv, void *userdata) {
	(void)userdata;

	GDBusConnection *conn = g_dbus_method_invocation_get_connection(inv);

	g_dbus_connection_unregister_object(conn, dbus_agent_object_id);
	ofono_remove_all_cards();

	g_object_unref(inv);
}

static void ofono_hf_audio_agent_method_call(GDBusConnection *conn, const gchar *sender,
		const gchar *path, const gchar *interface, const gchar *method, GVariant *params,
		GDBusMethodInvocation *invocation, void *userdata) {
	(void)conn;
	(void)sender;
	(void)path;
	(void)interface;
	(void)params;

	debug("oFono audio agent method call: %s.%s()", interface, method);

	if (strcmp(method, "NewConnection") == 0)
		ofono_agent_new_connection(invocation, userdata);
	else if (strcmp(method, "Release") == 0)
		ofono_agent_release(invocation, userdata);
	else
		warn("Unsupported oFono method: %s", method);

}

static const GDBusInterfaceVTable ofono_vtable = {
	.method_call = ofono_hf_audio_agent_method_call,
};

/**
 * Register to the oFono service.
 *
 * @return On success this function returns 0. Otherwise -1 is returned. */
int ofono_register(void) {

	const char *path = "/HandsfreeAudioAgent";
	GDBusConnection *conn = config.dbus;
	GDBusMessage *msg = NULL, *rep = NULL;
	GError *err = NULL;
	int ret = 0;

	if (!config.enable.hfp_ofono)
		goto final;

	debug("Registering oFono audio agent");
	if ((dbus_agent_object_id = g_dbus_connection_register_object(conn, path,
					(GDBusInterfaceInfo *)&ofono_iface_hf_audio_agent, &ofono_vtable,
					NULL, NULL, &err)) == 0)
		goto fail;

	msg = g_dbus_message_new_method_call(OFONO_SERVICE, "/",
			OFONO_IFACE_HF_AUDIO_MANAGER, "Register");

	GVariantBuilder options;

	g_variant_builder_init(&options, G_VARIANT_TYPE("ay"));
	g_variant_builder_add(&options, "y", OFONO_AUDIO_CODEC_CVSD);

	g_dbus_message_set_body(msg, g_variant_new("(oay)", path, &options));
	g_variant_builder_clear(&options);

	if ((rep = g_dbus_connection_send_message_with_reply_sync(conn, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, &err)) == NULL)
		goto fail;

	if (g_dbus_message_get_message_type(rep) == G_DBUS_MESSAGE_TYPE_ERROR) {
		g_dbus_message_to_gerror(rep, &err);
		goto fail;
	}

	ofono_get_all_cards();

	goto final;

fail:
	ret = -1;

final:
	if (msg != NULL)
		g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);
	if (err != NULL) {
		warn("Couldn't register oFono: %s", err->message);
		g_dbus_connection_unregister_object(conn, dbus_agent_object_id);
		g_error_free(err);
	}

	return ret;
}

/**
 * Callback for the CardAdded signal (emitted when phone is connected). */
static void ofono_signal_card_added(GDBusConnection *conn, const gchar *sender,
		const gchar *path, const gchar *interface, const gchar *signal, GVariant *params,
		void *userdata) {
	(void)conn;
	(void)path;
	(void)interface;
	(void)userdata;

	const gchar *signature = g_variant_get_type_string(params);
	const char *card = NULL;
	GVariantIter *properties = NULL;

	if (strcmp(signature, "(oa{sv})") != 0) {
		error("Invalid signature for %s: %s != %s", signal, signature, "(oa{sv})");
		return;
	}

	g_variant_get(params, "(&oa{sv})", &card, &properties);
	ofono_card_add(sender, card, properties);

	g_variant_iter_free(properties);
}

/**
 * Callback for the CardRemoved signal (emitted when phone is disconnected). */
static void ofono_signal_card_removed(GDBusConnection *conn, const gchar *sender,
		const gchar *path, const gchar *interface, const gchar *signal, GVariant *params,
		void *userdata) {
	(void)conn;
	(void)sender;
	(void)path;
	(void)interface;
	(void)userdata;

	const gchar *signature = g_variant_get_type_string(params);
	const char *card = NULL;

	if (strcmp(signature, "(o)") != 0) {
		error("Invalid signature for %s: %s != %s", signal, signature, "(o)");
		return;
	}

	g_variant_get(params, "(&o)", &card);

	debug("Removing oFono card: %s", card);

	bluealsa_devpool_mutex_lock();
	bluealsa_device_remove(card);
	bluealsa_devpool_mutex_unlock();

}

/**
 * Monitor oFono service availability.
 *
 * When oFono is properly shutdown, we are notified through the Release()
 * method. Here, we get the opportunity to perform some cleanup if oFono
 * was killed. */
static void ofono_signal_name_owner_changed(GDBusConnection *conn, const gchar *sender,
		const gchar *path, const gchar *interface, const gchar *signal, GVariant *params,
		void *userdata) {
	(void)conn;
	(void)sender;
	(void)path;
	(void)interface;
	(void)signal;
	(void)userdata;

	const char *name;
	const char *owner_old;
	const char *owner_new;

	g_variant_get(params, "(&s&s&s)", &name, &owner_old, &owner_new);

	if (owner_old != NULL && owner_old[0] != '\0') {
		g_dbus_connection_unregister_object(conn, dbus_agent_object_id);
		ofono_remove_all_cards();
	}
	if (owner_new != NULL && owner_new[0] != '\0')
		ofono_register();

}

/**
 * Subscribe to oFono signals.
 *
 * @return On success this function returns 0. Otherwise -1 is returned. */
int ofono_subscribe_signals(void) {

	if (!config.enable.hfp_ofono)
		return 0;

	GDBusConnection *conn = config.dbus;

	g_dbus_connection_signal_subscribe(conn, OFONO_SERVICE, OFONO_IFACE_HF_AUDIO_MANAGER,
			"CardAdded", NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
			ofono_signal_card_added, NULL, NULL);
	g_dbus_connection_signal_subscribe(conn, OFONO_SERVICE, OFONO_IFACE_HF_AUDIO_MANAGER,
			"CardRemoved", NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
			ofono_signal_card_removed, NULL, NULL);

	g_dbus_connection_signal_subscribe(conn, "org.freedesktop.DBus", "org.freedesktop.DBus",
			"NameOwnerChanged", NULL, OFONO_SERVICE, G_DBUS_SIGNAL_FLAGS_NONE,
			ofono_signal_name_owner_changed, NULL, NULL);

	return 0;
}
