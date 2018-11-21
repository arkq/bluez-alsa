/*
 * BlueALSA - ofono.c
 * Copyright (c) 2018 Thierry Bultel
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

/*
 * This is the implementation of a ofono backend for bluez-alsa
 * Namely, when Ofono is running on a system, it registers to bluez
 * as an HFP profile, which conflicts with the "--hfp-ag" and "--hpf-hf" modes
 * where bluez-alsa does the same thing
 * When Ofono is available, it has a DBus interface for clients to use the
 * audio stream of phone calls.
 * For more details, see https://github.com/rilmodem/ofono/blob/master/doc/handsfree-audio-api.txt
 * */

#include "ofono.h"
#include "ofono-iface.h"
#include "bluealsa.h"
#include "transport.h"
#include "shared/log.h"
#include "ctl.h"

#include <gio/gunixfdlist.h>
#include <errno.h>
#include <malloc.h>

#define HF_AUDIO_AGENT_PATH		"/HandsfreeAudioAgent"

#define GET_CARDS_SIGNATURE		"a(oa{sv})"
#define CARD_ADDED_SIGNATURE	"(oa{sv})"
#define CARD_REMOVED_SIGNATURE	"(o)"

#define HFP_AUDIO_CODEC_CVSD    0x01
#define HFP_AUDIO_CODEC_MSBC    0x02

#define OFONO_FAKE_DEV_ID		0xffff

/* static variables */

static guint card_added_subsc_id = 0;
static guint card_removed_subsc_id = 0;
static guint name_owner_changed_subsc_id = 0;
static guint agent_methods_id = 0;

/* forward static declarations */

static struct ba_transport * ofono_transport_new(
	const char *card,
	const char *dbus_owner,
	const char *dbus_path,
	const char *remote_address,
	enum bluetooth_profile profile
	);

static int ofono_agent_register();
static void ofono_card_new(	const char * dbus_sender, const char * dbus_path, GVariant * gpath , GVariantIter * gproperties);
static void ofono_card_remove_all();
static int ofono_socket_accept(int sock);

/* implementation */

/**
 * Asks Ofono for the cards it knows. */

static void ofono_get_cards() {
	GDBusConnection *conn = config.dbus;
	GDBusMessage *msg = NULL, *rep = NULL;
	GError *err = NULL;

	msg = g_dbus_message_new_method_call(OFONO_SERVICE, "/", HF_AUDIO_MANAGER_INTERFACE, "GetCards");
	if ((rep = g_dbus_connection_send_message_with_reply_sync(conn, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, &err)) == NULL)
		goto fail;

	if (g_dbus_message_get_message_type(rep) == G_DBUS_MESSAGE_TYPE_ERROR) {
		g_dbus_message_to_gerror(rep, &err);
		error("Failed to get cards from !\n");
		goto fail;
	}

	const gchar * signature = g_dbus_message_get_signature(rep);
	if (strcmp(signature,GET_CARDS_SIGNATURE) != 0) {
		error("%s: wrong reply signature %s", signature);
		goto fail;
	}

	const gchar *dbus_sender = g_dbus_message_get_sender(rep);
	GVariant * body = g_dbus_message_get_body(rep);

	GVariant *gpath;
	GVariantIter * gcards;
	GVariantIter * gproperties = NULL;

	g_variant_get(body, "(a(oa{sv}))", &gcards);
	while (g_variant_iter_next(gcards, "(&oa{sv})", &gpath, &gproperties)) {
		ofono_card_new(dbus_sender, "/" , gpath, gproperties);
	}

fail:
	if (msg != NULL)
		g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);

	return;
}

/*
 * Looks for a transport for the given card name
 * @return On success this function returns the matching new transport, otherwise a NULL pointer is returned. */

static struct ba_transport * ofono_transport_find(const char * path) {
	GHashTableIter iter_d, iter_t;
	struct ba_device *d;
	struct ba_transport *t = NULL;

	g_hash_table_iter_init(&iter_d, config.devices);
	while (g_hash_table_iter_next(&iter_d, NULL, (gpointer)&d)) {
		g_hash_table_iter_init(&iter_t, d->transports);
		while (g_hash_table_iter_next(&iter_t, NULL, (gpointer)&t)) {
			if (t->type == TRANSPORT_TYPE_SCO &&
				t->sco.is_ofono &&
				strcmp(t->sco.ofono.card, path) == 0) {
				debug("%s: found\n", path);
				goto done;
			}
		}
	}
done:
	return t;
}

/* release callback for the transport layer
 * This is called when the transport is deleted */

static int ofono_transport_release(struct ba_transport *t) {
	t->release = NULL;
	if (t->sco.ofono.card)
		free(t->sco.ofono.card);

	debug("done");
	return 0;
}

/* This will be called by ofono, after a card connect request
 * is sent and when the SCO audio link is available */

static void ofono_profile_new_connection(GDBusMethodInvocation * invocation, void * userdata) {
	debug("...");

	GUnixFDList *fd_list;

	GDBusMessage *msg = g_dbus_method_invocation_get_message(invocation);
	const gchar *sender = g_dbus_method_invocation_get_sender(invocation);
	const gchar *path = g_dbus_method_invocation_get_object_path(invocation);
	GVariant *params = g_dbus_method_invocation_get_parameters(invocation);

	GVariant * gcard;
    int fd;
    uint8_t codec;
	GError *err = NULL;

	const gchar *signature = g_variant_get_type_string(params);

	if (strcmp(signature,"(ohy)") != 0) {
		error("wrong method signature %s", signature);
		goto fail;
	}

	g_variant_get(params, "(&ohy)", &gcard, &fd, &codec);

	fd_list = g_dbus_message_get_unix_fd_list(msg);
	int fd2 = g_unix_fd_list_get(fd_list, 0, &err);

	debug("got card %s, fd %d, codec %d", gcard, fd2, codec);

	const char * card = (const char*)gcard;

	// retrieve the transport from the list
	struct ba_transport * transport = ofono_transport_find(card);
	if (!transport) {
		debug("unknown transport for %s", card);
		goto fail;
	}

	transport->bt_fd = fd2;
	transport->codec = codec;
	ofono_socket_accept(fd2);
	bluealsa_ctl_event(BA_EVENT_TRANSPORT_ADDED);

	g_dbus_method_invocation_return_value(invocation, NULL);
	return;

fail:
	g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
		G_DBUS_ERROR_INVALID_ARGS, "Unable to connect profile");
	return ;

}

/* callback for the DBus Release method
 * It is called when Ofono is properly shutdown
 */

static void ofono_profile_release(GDBusMethodInvocation * invocation, void *userdata) {
	debug("...");
	/* The agent has been released by Ofono (typically, because Ofono has exited)*/
	ofono_card_remove_all();
}


/* callback for our exposed DBus interface methods
*/

static void ofono_profile_method_call(
	GDBusConnection *conn,
	const gchar *sender,
	const gchar *path,
	const gchar *interface,
	const gchar *method,
	GVariant *params,
	GDBusMethodInvocation *invocation,
	void *userdata) {

	(void)conn;
	(void)sender;
	(void)path;
	(void)interface;
	(void)params;

	debug("Ofono Profile method call: %s.%s()", interface, method);

	if (strcmp(method, "NewConnection") == 0)
		ofono_profile_new_connection(invocation, userdata);
	else if (strcmp(method, "Release") == 0)
		ofono_profile_release(invocation, userdata);
	else
		warn("Unsupported profile method: %s", method);

}


/* Disconnects from a card
 * This is basically called when no PCM client is connected, in order to save
 * bandwidth for Bluetooth
 * @return On success this function returns 0. Otherwise -1 is returned. */

static int ofono_card_disconnect(struct ba_transport * transport) {
	struct ofono * ofono = &transport->sco.ofono;
	const char * card = ofono->card;
	debug("... %s", card);

	ofono->connect_pending = false;

	if (transport->bt_fd == -1) {
		debug("%s-> already down.", card);
		return 0;
	}

	shutdown(transport->bt_fd, SHUT_RDWR);
	close(transport->bt_fd);

	transport->bt_fd = -1;
	transport->codec = HFP_CODEC_UNDEFINED;

	bluealsa_ctl_event(BA_EVENT_TRANSPORT_REMOVED);
	debug("%s -> DONE", card);

	return 0;
}


/*
 * Removes a card with the given name. The associated transport is deleted as well
 * */
static void ofono_card_remove(const char * path) {

	GHashTableIter iter_d, iter_t;
	struct ba_device *d;
	struct ba_transport *t;

	debug("%s...", path);

	pthread_mutex_lock(&config.devices_mutex);

	g_hash_table_iter_init(&iter_d, config.devices);
	while (g_hash_table_iter_next(&iter_d, NULL, (gpointer)&d)) {

		if (strcmp(d->name, path) != 0) {
			continue;
		}

		/* will call device_free, that will call transport_free */
		g_hash_table_iter_remove(&iter_d);
		if (g_hash_table_size(config.devices) == 0)
			goto done;
	}

done:
	pthread_mutex_unlock(&config.devices_mutex);
	debug("%s... DONE", path);
}

/*
 * Forgets all Ofono cards and deletes associated transports
 * */

static void ofono_card_remove_all() {
	GHashTableIter iter_d, iter_t;
	struct ba_device *d;
	struct ba_transport *t;

	debug("...");

	pthread_mutex_lock(&config.devices_mutex);

	g_hash_table_iter_init(&iter_d, config.devices);
	while (g_hash_table_iter_next(&iter_d, NULL, (gpointer)&d)) {

		if (d->hci_dev_id != OFONO_FAKE_DEV_ID)
			continue;

		debug("remove card %s", d->name);
		g_hash_table_iter_remove(&iter_d);
		if (g_hash_table_size(config.devices) == 0)
			goto done;
	}

done:
	pthread_mutex_unlock(&config.devices_mutex);
}


/*
 * Asks Ofono to connect to a card. It should in return invoke our NewConnection method
 * @return On success this function returns 0. Otherwise -1 is returned. */


static int ofono_card_connect(struct ba_transport * transport) {
	GDBusConnection *conn = config.dbus;
	GDBusMessage *msg = NULL, *rep = NULL;
	GError *err = NULL;
	struct ofono * ofono = &transport->sco.ofono;

	const char * path = ofono->card;
	debug("%s...", path);

	if (ofono->connect_pending) {
		debug("connect already pending");
		return 0;
	}

	ofono->connect_pending = false;

	msg = g_dbus_message_new_method_call(OFONO_SERVICE, path, HF_AUDIO_CARD_INTERFACE, "Connect");

	if ((rep = g_dbus_connection_send_message_with_reply_sync(conn, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, &err)) == NULL)
		goto fail;

	if (g_dbus_message_get_message_type(rep) == G_DBUS_MESSAGE_TYPE_ERROR) {
		g_dbus_message_to_gerror(rep, &err);
		error("Failed to connect to card %s: %s !\n", path, err->message);
		goto fail;
	}

	return 0;

fail:
	if (msg != NULL)
		g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);
	return -1;

}

/* Adds a new card. This can be called upon the CardAdded signal, or when retrieving the list of cards at startup */

static void ofono_card_new(	const char * dbus_sender, const char * dbus_path, GVariant * gpath , GVariantIter * gproperties) {
	debug("gpath = %s", gpath);

	gchar *key = NULL;

	GVariant *value = NULL;
	const gchar * remote_address = NULL;
	const gchar * local_address = NULL;
	const gchar * type = NULL;

	while (g_variant_iter_next(gproperties, "{&sv}", &key, &value)) {

		if (strcmp(key,"RemoteAddress") == 0) {
			remote_address = g_variant_get_string(value, NULL);
			debug("\t%s:%s", key, remote_address);
		}
		if (strcmp(key,"LocalAddress") == 0) {
			local_address = g_variant_get_string(value, NULL);
			debug("\t%s:%s", key, local_address);
		}
		if (strcmp(key,"Type") == 0) {
			type = g_variant_get_string(value, NULL);
			debug("\t%s:%s", key, type);
		}
		g_variant_unref(value);
		value = NULL;
		}

	enum bluetooth_profile profile;

	if (strcmp(type, "gateway") == 0)
		profile = BLUETOOTH_PROFILE_HFP_AG;
	else if (strcmp(type, "handsfree") == 0)
		profile = BLUETOOTH_PROFILE_HFP_HF;
	else {
		error("unsupported profile type %s", type);
		goto fail;
	}

	struct ba_transport * transport = NULL;

	transport = ofono_transport_new( (char*)gpath, dbus_sender, dbus_path, remote_address, BLUETOOTH_PROFILE_HFP_HF);
	if (transport == NULL)
		goto fail;

	transport_set_state(transport, TRANSPORT_ACTIVE);

fail:

	if (value != NULL)
		g_variant_unref(value);
}


/* callback for the CardAdded signal (that raises when a phone is connected) */

static void ofono_signal_card_added(GDBusConnection *conn, const gchar *sender,
		const gchar *path, const gchar *interface, const gchar *signal, GVariant *params,
		void *userdata) {
	(void)conn;
	(void)sender;
	(void)path;
	(void)interface;
	(void)userdata;

	debug("sender %s, path %s, interface %s, signal %s",
			sender, path, interface, signal);

	const gchar *signature = g_variant_get_type_string(params);

	if (strcmp(signature, CARD_ADDED_SIGNATURE) != 0) {
		error("Invalid signature for %s: %s != %s", signal, signature, CARD_ADDED_SIGNATURE);
		goto fail;
	}
	GVariant * gpath = NULL;
	GVariantIter * gproperties = NULL;

	g_variant_get(params, "(&oa{sv})", &gpath, &gproperties);
	ofono_card_new(sender, path, gpath, gproperties);

fail:
	if (gproperties)
		g_variant_iter_free(gproperties);
	return;

}

/* callback for the CardRemoved signal (that raises when a phone is disconnected) */

static void ofono_signal_card_removed(GDBusConnection *conn, const gchar *sender,
		const gchar *path, const gchar *interface, const gchar *signal, GVariant *params,
		void *userdata) {
	debug("sender %s, path %s, interface %s, signal %s",
			sender, path, interface, signal);

	const gchar *signature = g_variant_get_type_string(params);

	if (strcmp(signature, CARD_REMOVED_SIGNATURE) != 0) {
		error("Invalid signature for %s: %s != %s", signal, signature, CARD_ADDED_SIGNATURE);
		goto fail;
	}

	GVariant * gpath = NULL;

	g_variant_get(params, "(&o)", &gpath);
	debug("gpath = %s", gpath);

	ofono_card_remove((char*)gpath);

fail:
	return;
}

/* Monitor the Ofono service availability. When Ofono is properly shutdown,
 * we are notified through the Release() method.
 * We get here the opportunity to perform some cleanup if it is killed */

static void ofono_signal_name_owner_changed(GDBusConnection *conn, const gchar *sender,
		const gchar *path, const gchar *interface, const gchar *signal, GVariant *params,
		void *userdata) {

	gchar * name, *old_owner, *new_owner;
	g_variant_get(params, "(&s&s&s)", &name, &old_owner, &new_owner);

	if (strcmp(name, OFONO_SERVICE) != 0)
		return;

	if (old_owner && *old_owner) {
		debug("OFONO DISAPPEARED");
		ofono_card_remove_all();
	}

    if (new_owner && *new_owner) {
		debug("OFONO APPEARED");
		ofono_agent_register();
    }
}

/*
 * Creates transport of type TRANSPORT_TYPE_OFONO_SCO
 * It will be created with an unset codec, which is the condition for it
 * to be hidden to clients. (The codec will be set when the phone call starts)
 * @return On success this function returns a new transport, otherwise a NULL pointer is returned. */


static struct ba_transport * ofono_transport_new(
	const char * card,
	const char *dbus_owner,
	const char *dbus_path,
	const char *remote_address,
	enum bluetooth_profile profile
	) {
	struct ba_transport * transport = NULL;
	struct ba_device *d = NULL;

	debug("owner %s, path %s, card %s", dbus_owner, dbus_path, card);

	bdaddr_t addr;
	str2ba(remote_address, &addr);

	d = device_new(OFONO_FAKE_DEV_ID, &addr, card);
	g_hash_table_insert(config.devices, g_strdup(card), d);

	if ((transport = transport_new(d, TRANSPORT_TYPE_SCO, dbus_owner, dbus_path, profile, HFP_CODEC_UNDEFINED)) == NULL) {
		goto fail;
	}

	struct ofono * ofono = &transport->sco.ofono;

	ofono->card = strdup(card);
	ofono->connect_pending = false;

	transport_sco_init(transport);

	transport->bt_fd = -1; // will be set later, at connection time

	/* empiric values taken from the pulseaudio code */
	transport->mtu_read = 48;
	transport->mtu_write = 48;

	transport->sco.is_ofono = true;
	transport->sco.acquire = ofono_card_connect;
	transport->sco.release = ofono_card_disconnect;

	transport->release = ofono_transport_release;

	return transport;

fail:
	if (transport)
		transport_free(transport);

	return NULL;
}

/*
 * This helper function makes the authorization of the connection
 * @return On success this function returns 0. Otherwise an negative error code is returned. */

static int ofono_socket_accept(int sock)
{
    char c;
    struct pollfd pfd;

    if (sock < 0)
        return -ENOTCONN;

    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = sock;
    pfd.events = POLLOUT;

    if (poll(&pfd, 1, 0) < 0)
        return -errno;

    /*
     * If socket already writable then it is not in defer setup state,
     * otherwise it needs to be read to authorize the connection.
     */
    if ((pfd.revents & POLLOUT))
        return 0;

    /* Enable socket by reading 1 byte */
    if (read(sock, &c, 1) < 0)
        return -errno;

    return 0;
}


/*
 * Unregisters us from Ofono
 * @return On success this function returns 0. Otherwise -1 is returned. */


static int ofono_agent_unregister() {
	GDBusMessage *msg = NULL, *rep = NULL;
	GDBusConnection *conn = config.dbus;
	GError *err = NULL;
	const char *path = HF_AUDIO_AGENT_PATH;

	int ret = -1;

	msg = g_dbus_message_new_method_call(OFONO_SERVICE, "/", HF_AUDIO_MANAGER_INTERFACE, "Unregister");
	g_dbus_message_set_body(msg, g_variant_new("(o)", path));

	if ((rep = g_dbus_connection_send_message_with_reply_sync(conn, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, &err)) == NULL)
		goto fail;

	if (g_dbus_message_get_message_type(rep) == G_DBUS_MESSAGE_TYPE_ERROR) {
		g_dbus_message_to_gerror(rep, &err);
		error("Failed to unregister from Ofono !\n");
		goto fail;
	}

	ret = 0;

fail:

	if (msg != NULL)
		g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);
	if (err != NULL) {
		warn("Couldn't unregister profile: %s", err->message);
		g_error_free(err);
	}

	return ret;
}

/*
 * Registers as an agent to Ofono
 * @return On success this function returns 0. Otherwise -1 is returned. */

static int ofono_agent_register() {
	GDBusConnection *conn = config.dbus;
	GDBusMessage *msg = NULL, *rep = NULL;
	GError *err = NULL;
	int ret = -1;

	const char *path = HF_AUDIO_AGENT_PATH;

	msg = g_dbus_message_new_method_call(OFONO_SERVICE, "/", HF_AUDIO_MANAGER_INTERFACE, "Register");

	GVariantBuilder options;

	g_variant_builder_init(&options, G_VARIANT_TYPE("ay"));
	g_variant_builder_add(&options, "y", HFP_AUDIO_CODEC_CVSD);
	/* MSBC is not supported yet */
/*	g_variant_builder_add(&options, "y", HFP_AUDIO_CODEC_MSBC);*/

	g_dbus_message_set_body(msg, g_variant_new("(oay)", path, &options));
	g_variant_builder_clear(&options);

	if ((rep = g_dbus_connection_send_message_with_reply_sync(conn, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, &err)) == NULL)
		goto fail;

	if (g_dbus_message_get_message_type(rep) == G_DBUS_MESSAGE_TYPE_ERROR) {
		g_dbus_message_to_gerror(rep, &err);
		error("Failed to register to Ofono !\n");
		goto fail;
	}

	ofono_get_cards();

	debug("Agent Successfully registered to Ofono");
	ret = 0;

fail:

	if (msg != NULL)
		g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);
	if (err != NULL) {
		warn("Couldn't register profile: %s", err->message);
		g_dbus_connection_unregister_object(conn, agent_methods_id);
		g_error_free(err);
	}

	return ret;
}


static const GDBusInterfaceVTable profile_vtable = {
	.method_call = ofono_profile_method_call,
};


/*
 * Prepares the agent method for Ofono,
 * registers to the interesting signals and calls ofono_agent_register
 * @return On success this function returns 0. Otherwise -1 is returned. */

int ofono_register(void) {
	GDBusConnection *conn = config.dbus;
	int ret = -1;
	GError *err = NULL;

	if (!config.enable.hfp_ofono)
		goto fail;

	debug("Adding Ofono profile methods ...");

	if ((agent_methods_id = g_dbus_connection_register_object(conn, HF_AUDIO_AGENT_PATH,
					&ofono_iface_profile, &profile_vtable,
					NULL, NULL, &err)) == 0) {
		error("Failed to add ofono methods");
		goto fail;
	}

	card_added_subsc_id = g_dbus_connection_signal_subscribe(conn, OFONO_SERVICE, HF_AUDIO_MANAGER_INTERFACE,
			"CardAdded", NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
			ofono_signal_card_added, NULL, NULL);

	card_removed_subsc_id = g_dbus_connection_signal_subscribe(conn, OFONO_SERVICE, HF_AUDIO_MANAGER_INTERFACE,
			"CardRemoved", NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
			ofono_signal_card_removed, NULL, NULL);

	name_owner_changed_subsc_id = g_dbus_connection_signal_subscribe(conn, "org.freedesktop.DBus", "org.freedesktop.DBus",
			"NameOwnerChanged", NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
			ofono_signal_name_owner_changed, NULL, NULL);

	if (ofono_agent_register() != 0)
		goto fail;

	ret = 0;
fail:
	return ret;
}

/*
 * Global cleanup
 * Unregisters from the signals, remove interface methods
 * and calls ofono_agent_unregister
 * @return On success this function returns 0. Otherwise -1 is returned. */

int ofono_unregister(void) {
	GDBusConnection *conn = config.dbus;
	int ret = -1;

	if (!config.enable.hfp_ofono)
		goto fail;

	g_dbus_connection_signal_unsubscribe(conn, card_added_subsc_id);
	g_dbus_connection_signal_unsubscribe(conn, card_removed_subsc_id);
	g_dbus_connection_signal_unsubscribe(conn, name_owner_changed_subsc_id);

	g_dbus_connection_unregister_object(conn, agent_methods_id);

	if (ofono_agent_unregister() != 0)
		goto fail;

	ret = 0;
fail:
	return ret;
}

