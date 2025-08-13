/*
 * BlueALSA - ofono.c
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
 * Copyright (c) 2018 Thierry Bultel
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 * When oFono is running on a system, it registers itself to BlueZ as an HFP
 * profile, which conflicts with our internal "--hfp-ag" and "--hfp-hf" ones.
 * This file is an implementation of the oFono back-end for bluez-alsa.
 *
 * For more details, see:
 * https://github.com/rilmodem/ofono/blob/master/doc/handsfree-audio-api.txt
 *
 */

#include "ofono.h"

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci_lib.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib-object.h>
#include <glib.h>

#include "ba-adapter.h"
#include "ba-config.h"
#include "ba-device.h"
#include "ba-transport.h"
#include "ba-transport-pcm.h"
#include "bluealsa-dbus.h"
#include "dbus.h"
#include "hci.h"
#include "hfp.h"
#include "ofono-iface.h"
#include "utils.h"
#include "shared/defs.h"
#include "shared/log.h"
#include "shared/rt.h"

/**
 * Lookup data associated with oFono card. */
struct ofono_card_data {
	char card[64];
	int hci_dev_id;
	bdaddr_t bt_addr;
	/* if true, card is an HFP AG */
	bool is_gateway;
	/* object path of a modem associated with this card */
	char modem_path[64];
};

static GHashTable *ofono_card_data_map = NULL;
static const char *dbus_agent_object_path = "/org/bluez/HFP/oFono";
static OrgOfonoHandsfreeAudioAgentSkeleton *dbus_hf_agent = NULL;

/**
 * Ask oFono to connect to a card. */
static int ofono_acquire_bt_sco(struct ba_transport *t) {

	GDBusMessage *msg = NULL, *rep = NULL;
	GError *err = NULL;
	uint8_t codec;
	int fd = -1;
	int ret = -1;

	debug("Requesting new oFono SCO link: %s", t->sco.ofono_dbus_path_card);
	msg = g_dbus_message_new_method_call(t->bluez_dbus_owner,
			t->sco.ofono_dbus_path_card, OFONO_IFACE_HF_AUDIO_CARD, "Acquire");

	struct timespec now;
	struct timespec delay = {
		.tv_nsec = HCI_SCO_CLOSE_CONNECT_QUIRK_DELAY * 1000000 };

	gettimestamp(&now);
	timespecadd(&t->sco.closed_at, &delay, &delay);
	if (difftimespec(&now, &delay, &delay) > 0) {
		info("SCO link close-connect quirk delay: %d ms",
				(int)(delay.tv_nsec / 1000000));
		nanosleep(&delay, NULL);
	}

	if ((rep = g_dbus_connection_send_message_with_reply_sync(config.dbus, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, &err)) == NULL ||
			g_dbus_message_to_gerror(rep, &err))
		goto fail;

	GVariant *body = g_dbus_message_get_body(rep);
	g_variant_get(body, "(hy)", NULL, &codec);

	GUnixFDList *fd_list = g_dbus_message_get_unix_fd_list(rep);
	if ((fd = g_unix_fd_list_get(fd_list, 0, &err)) == -1)
		goto fail;

#if ENABLE_MSBC
	if (codec != ba_transport_get_codec(t)) {
		/* Although this connection has succeeded, it is not the codec expected
		 * by the client. So we have to return an error ... */
		error("Rejecting oFono SCO link: %s", "Codec mismatch");
		/* ... but still update the codec ready for next client request. */
		ba_transport_set_codec(t, codec);
		if (fd != -1) {
			shutdown(fd, SHUT_RDWR);
			close(fd);
		}
		goto fail;
	}
#endif

	t->bt_fd = fd;
	t->mtu_read = t->mtu_write = hci_sco_get_mtu(fd, t->d->a);
	ba_transport_set_codec(t, codec);

	debug("New oFono SCO link (codec: %#x): %d", codec, fd);
	ret = 0;

fail:
	if (msg != NULL)
		g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);
	if (err != NULL) {
		error("Couldn't establish oFono SCO link: %s", err->message);
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

	debug("Closing oFono SCO link: %d", t->bt_fd);

	shutdown(t->bt_fd, SHUT_RDWR);
	close(t->bt_fd);
	t->bt_fd = -1;

	/* Keep the time-stamp when the SCO link has been closed. It will be used
	 * for calculating close-connect quirk delay in the acquire function. */
	gettimestamp(&t->sco.closed_at);

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
		enum ba_transport_profile profile,
		const char *dbus_owner,
		const char *dbus_path_card,
		const char *dbus_path_modem) {

	struct ba_transport *t;
	int err;

	if ((t = ba_transport_new_sco(device, profile, dbus_owner, dbus_path_card, -1)) == NULL)
		return NULL;

	if ((t->sco.ofono_dbus_path_card = strdup(dbus_path_card)) == NULL)
		goto fail;
	if ((t->sco.ofono_dbus_path_modem = strdup(dbus_path_modem)) == NULL)
		goto fail;

	t->acquire = ofono_acquire_bt_sco;
	t->release = ofono_release_bt_sco;

	return t;

fail:
	err = errno;
	ba_transport_unref(t);
	errno = err;
	return NULL;
}

/**
 * Lookup a transport associated with oFono card data. */
static struct ba_transport *ofono_transport_lookup(
		const struct ofono_card_data *ocd) {

	struct ba_adapter *a = NULL;
	struct ba_device *d = NULL;
	struct ba_transport *t = NULL;

	if ((a = ba_adapter_lookup(ocd->hci_dev_id)) == NULL)
		goto fail;
	if ((d = ba_device_lookup(a, &ocd->bt_addr)) == NULL)
		goto fail;
	t = ba_transport_lookup(d, ocd->card);

fail:
	if (a != NULL)
		ba_adapter_unref(a);
	if (d != NULL)
		ba_device_unref(d);
	return t;
}

/**
 * Lookup a transport associated with oFono card.
 *
 * @param card A path associated with oFono card.
 * @return On success this function returns a transport associated with
 *   a given oFono card path. Otherwise, NULL is returned. */
static struct ba_transport *ofono_transport_lookup_card(const char *card) {

	struct ofono_card_data *ocd;
	if ((ocd = g_hash_table_lookup(ofono_card_data_map, card)) != NULL)
		return ofono_transport_lookup(ocd);

	error("Couldn't lookup oFono card data: %s", card);
	return NULL;
}

/**
 * Lookup a transport associated with oFono modem.
 *
 * @param modem A path associated with oFono modem.
 * @return On success this function returns a transport associated with
 *   a given oFono modem path. Otherwise, NULL is returned. */
static struct ba_transport *ofono_transport_lookup_modem(const char *modem) {

	GHashTableIter iter;
	struct ofono_card_data *ocd;

	g_hash_table_iter_init(&iter, ofono_card_data_map);
	while (g_hash_table_iter_next(&iter, NULL, (void *)&ocd)) {
		if (strcmp(ocd->modem_path, modem) == 0)
			return ofono_transport_lookup(ocd);
	}

	return NULL;
}

#if ENABLE_MSBC
static void ofono_new_connection_finish(GObject *source, GAsyncResult *result,
		void *userdata) {
	(void)userdata;

	GDBusMessage *rep;
	GError *err = NULL;

	if ((rep = g_dbus_connection_send_message_with_reply_finish(
					G_DBUS_CONNECTION(source), result, &err)) != NULL)
		g_dbus_message_to_gerror(rep, &err);

	if (rep != NULL)
		g_object_unref(rep);
	if (err != NULL) {
		error("Couldn't establish oFono SCO link: %s", err->message);
		g_error_free(err);
	}

}

/**
 * Ask oFono to create an HFP codec connection.
 *
 * Codec selection can take a long time with oFono (up to 20 seconds with
 * some devices) so we make the request asynchronously. oFono will invoke
 * the HandsFreeAudioAgent NewConnection method when the codec selection is
 * complete. */
static void ofono_new_connection_request(struct ba_transport *t) {

	GDBusMessage *msg;

	debug("Requesting new oFono SCO link: %s", t->sco.ofono_dbus_path_card);
	msg = g_dbus_message_new_method_call(t->bluez_dbus_owner,
			t->sco.ofono_dbus_path_card, OFONO_IFACE_HF_AUDIO_CARD, "Connect");

	g_dbus_connection_send_message_with_reply(config.dbus, msg,
			G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL,
			ofono_new_connection_finish, NULL);

	g_object_unref(msg);

}
#endif

/**
 * Link oFono card with a modem. */
static int ofono_card_link_modem(struct ofono_card_data *ocd) {

	GDBusMessage *msg = NULL, *rep = NULL;
	GError *err = NULL;
	int ret = -1;

	msg = g_dbus_message_new_method_call(OFONO_SERVICE, "/",
			OFONO_IFACE_MANAGER, "GetModems");

	if ((rep = g_dbus_connection_send_message_with_reply_sync(config.dbus, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, &err)) == NULL ||
			g_dbus_message_to_gerror(rep, &err))
		goto fail;

	GVariant *body = g_dbus_message_get_body(rep);

	GVariantIter *modems;
	GVariantIter *properties;
	const char *modem;

	g_variant_get(body, "(a(oa{sv}))", &modems);
	while (g_variant_iter_next(modems, "(&oa{sv})", &modem, &properties)) {

		bdaddr_t bt_addr = { 0 };
		bool is_powered = false;
		bool is_bt_device = false;
		const char *serial = "";
		const char *key;
		GVariant *value;

		while (g_variant_iter_next(properties, "{&sv}", &key, &value)) {
			if (strcmp(key, "Powered") == 0 &&
					g_variant_validate_value(value, G_VARIANT_TYPE_BOOLEAN, key))
				is_powered = g_variant_get_boolean(value);
			else if (strcmp(key, "Type") == 0 &&
					g_variant_validate_value(value, G_VARIANT_TYPE_STRING, key)) {
				const char *type = g_variant_get_string(value, NULL);
				if (strcmp(type, OFONO_MODEM_TYPE_HFP) == 0 ||
						strcmp(type, OFONO_MODEM_TYPE_SAP) == 0)
					is_bt_device = true;
			}
			else if (strcmp(key, "Serial") == 0 &&
					g_variant_validate_value(value, G_VARIANT_TYPE_STRING, key))
				serial = g_variant_get_string(value, NULL);
			g_variant_unref(value);
		}

		if (is_bt_device)
			str2ba(serial, &bt_addr);

		g_variant_iter_free(properties);

		if (!is_powered)
			continue;

		/* In case of HFP AG, we are looking for a modem which is not a BT device.
		 * Unfortunately, oFono does not link card (Bluetooth HF device) with a
		 * particular modem. In case where more than one card is connected oFono
		 * uses all of them for call notification... However, in our setup we need
		 * a 1:1 mapping between card and modem. So, we will link the first modem
		 * which is not a BT device.
		 *
		 * TODO: Find a better way to link oFono card with a modem. */
		if (ocd->is_gateway && is_bt_device)
			continue;

		/* In case of HFP HF, we are looking for a modem which is a BT device and
		 * its serial number matches with the card BT address. */
		if (!ocd->is_gateway &&
				!(is_bt_device && bacmp(&bt_addr, &ocd->bt_addr) == 0))
			continue;

		debug("Linking oFono card with modem: %s", modem);
		strncpy(ocd->modem_path, modem, sizeof(ocd->modem_path) - 1);
		ocd->modem_path[sizeof(ocd->modem_path) - 1] = '\0';
		ret = 0;
		break;

	}

	g_variant_iter_free(modems);

fail:
	if (msg != NULL)
		g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);
	if (err != NULL) {
		error("Couldn't get oFono modems: %s", err->message);
		g_error_free(err);
	}

	return ret;
}

#define OFONO_CALL_VOLUME_NONE       (0)
#define OFONO_CALL_VOLUME_SPEAKER    (1 << 0)
#define OFONO_CALL_VOLUME_MICROPHONE (1 << 1)

static unsigned int ofono_call_volume_property_sync(struct ba_transport *t,
		const char *property, GVariant *value) {

	struct ba_transport_pcm *spk = &t->sco.pcm_spk;
	struct ba_transport_pcm *mic = &t->sco.pcm_mic;
	unsigned int mask = OFONO_CALL_VOLUME_NONE;

	if (strcmp(property, "Muted") == 0 &&
			g_variant_validate_value(value, G_VARIANT_TYPE_BOOLEAN, property)) {

		if (t->profile & BA_TRANSPORT_PROFILE_MASK_AG &&
				mic->soft_volume) {
			debug("Skipping SCO microphone mute update: %s", "Software volume enabled");
			goto final;
		}

		bool muted = g_variant_get_boolean(value);
		debug("Updating SCO microphone mute: %s", muted ? "true" : "false");
		mask |= OFONO_CALL_VOLUME_MICROPHONE;

		pthread_mutex_lock(&mic->mutex);
		ba_transport_pcm_volume_set(&mic->volume[0], NULL, &muted, NULL);
		pthread_mutex_unlock(&mic->mutex);

	}
	else if (strcmp(property, "SpeakerVolume") == 0 &&
			g_variant_validate_value(value, G_VARIANT_TYPE_BYTE, property)) {
		/* received volume is in range [0, 100] */

		if (t->profile & BA_TRANSPORT_PROFILE_MASK_AG &&
				spk->soft_volume) {
			debug("Skipping SCO speaker volume update: %s", "Software volume enabled");
			goto final;
		}

		uint8_t volume = g_variant_get_byte(value) * HFP_VOLUME_GAIN_MAX / 100;
		int level = ba_transport_pcm_volume_range_to_level(volume, HFP_VOLUME_GAIN_MAX);
		debug("Updating SCO speaker volume: %u [%.2f dB]", volume, 0.01 * level);
		mask |= OFONO_CALL_VOLUME_SPEAKER;

		pthread_mutex_lock(&spk->mutex);
		ba_transport_pcm_volume_set(&spk->volume[0], &level, NULL, NULL);
		pthread_mutex_unlock(&spk->mutex);

	}
	else if (strcmp(property, "MicrophoneVolume") == 0 &&
			g_variant_validate_value(value, G_VARIANT_TYPE_BYTE, property)) {
		/* received volume is in range [0, 100] */

		if (t->profile & BA_TRANSPORT_PROFILE_MASK_AG &&
				mic->soft_volume) {
			debug("Skipping SCO microphone volume update: %s", "Software volume enabled");
			goto final;
		}

		uint8_t volume = g_variant_get_byte(value) * HFP_VOLUME_GAIN_MAX / 100;
		int level = ba_transport_pcm_volume_range_to_level(volume, HFP_VOLUME_GAIN_MAX);
		debug("Updating SCO microphone volume: %u [%.2f dB]", volume, 0.01 * level);
		mask |= OFONO_CALL_VOLUME_MICROPHONE;

		pthread_mutex_lock(&mic->mutex);
		ba_transport_pcm_volume_set(&mic->volume[0], &level, NULL, NULL);
		pthread_mutex_unlock(&mic->mutex);

	}

final:
	return mask;
}

/**
 * Get all oFono call volume properties and update transport volumes. */
static int ofono_call_volume_get_properties(struct ba_transport *t) {

	GDBusMessage *msg = NULL, *rep = NULL;
	GError *err = NULL;
	int ret = -1;

	msg = g_dbus_message_new_method_call(t->bluez_dbus_owner,
			t->sco.ofono_dbus_path_modem, OFONO_IFACE_CALL_VOLUME, "GetProperties");

	if ((rep = g_dbus_connection_send_message_with_reply_sync(config.dbus, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, &err)) == NULL ||
			g_dbus_message_to_gerror(rep, &err))
		goto fail;

	GVariantIter *properties;
	g_variant_get(g_dbus_message_get_body(rep), "(a{sv})", &properties);

	const char *property;
	GVariant *value;

	unsigned int mask = OFONO_CALL_VOLUME_NONE;
	while (g_variant_iter_next(properties, "{&sv}", &property, &value)) {
		mask |= ofono_call_volume_property_sync(t, property, value);
		g_variant_unref(value);
	}

	if (mask & OFONO_CALL_VOLUME_SPEAKER)
		bluealsa_dbus_pcm_update(&t->sco.pcm_spk, BA_DBUS_PCM_UPDATE_VOLUME);
	if (mask & OFONO_CALL_VOLUME_MICROPHONE)
		bluealsa_dbus_pcm_update(&t->sco.pcm_mic, BA_DBUS_PCM_UPDATE_VOLUME);

	g_variant_iter_free(properties);

	ret = 0;

fail:
	if (msg != NULL)
		g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);
	if (err != NULL) {
		error("Couldn't get oFono call volume: %s", err->message);
		g_error_free(err);
	}

	return ret;
}

/**
 * Set oFono call volume property. */
static int ofono_call_volume_set_property(struct ba_transport *t,
		const char *property, GVariant *value, GError **error) {

	GDBusMessage *msg = NULL, *rep = NULL;
	int ret = -1;

	msg = g_dbus_message_new_method_call(t->bluez_dbus_owner,
			t->sco.ofono_dbus_path_modem, OFONO_IFACE_CALL_VOLUME, "SetProperty");

	g_dbus_message_set_body(msg, g_variant_new("(sv)", property, value));

	if ((rep = g_dbus_connection_send_message_with_reply_sync(config.dbus, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, error)) == NULL ||
			g_dbus_message_to_gerror(rep, error))
		goto fail;

	ret = 0;

fail:
	if (msg != NULL)
		g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);
	return ret;
}

/**
 * Add new oFono card (phone). */
static void ofono_card_add(const char *dbus_sender, const char *card,
		GVariantIter *properties) {

	struct ba_adapter *a = NULL;
	struct ba_device *d = NULL;
	struct ba_transport *t = NULL;

	enum ba_transport_profile profile = BA_TRANSPORT_PROFILE_NONE;
	struct ofono_card_data *ocd = NULL;
	const char *key = NULL;
	GVariant *value = NULL;
	bdaddr_t addr_dev = { 0 };
	bdaddr_t addr_hci = { 0 };
	int hci_dev_id = -1;

	while (g_variant_iter_next(properties, "{&sv}", &key, &value)) {
		if (strcmp(key, "RemoteAddress") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_STRING, key))
			str2ba(g_variant_get_string(value, NULL), &addr_dev);
		else if (strcmp(key, "LocalAddress") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_STRING, key)) {
			str2ba(g_variant_get_string(value, NULL), &addr_hci);
			hci_dev_id = hci_get_route(&addr_hci);
		}
		else if (strcmp(key, "Type") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_STRING, key)) {
			const char *type = g_variant_get_string(value, NULL);
			if (strcmp(type, OFONO_AUDIO_CARD_TYPE_AG) == 0)
				profile = BA_TRANSPORT_PROFILE_HFP_AG;
			else if (strcmp(type, OFONO_AUDIO_CARD_TYPE_HF) == 0)
				profile = BA_TRANSPORT_PROFILE_HFP_HF;
			else {
				error("Unsupported profile type: %s", type);
				goto fail;
			}
		}
		g_variant_unref(value);
		value = NULL;
	}

	debug("Adding new oFono card: %s", card);

	if ((a = ba_adapter_lookup(hci_dev_id)) == NULL) {
		error("Couldn't lookup adapter: hci%d: %s", hci_dev_id, strerror(errno));
		goto fail;
	}

	if ((d = ba_device_lookup(a, &addr_dev)) == NULL &&
			(d = ba_device_new(a, &addr_dev)) == NULL) {
		error("Couldn't create new device: %s", strerror(errno));
		goto fail;
	}

	if ((ocd = malloc(sizeof(*ocd))) == NULL) {
		error("Couldn't create oFono card data: %s", strerror(errno));
		goto fail;
	}

	ocd->hci_dev_id = hci_dev_id;
	ocd->bt_addr = addr_dev;
	ocd->is_gateway = profile & BA_TRANSPORT_PROFILE_MASK_AG;
	strncpy(ocd->card, card, sizeof(ocd->card) - 1);
	ocd->card[sizeof(ocd->card) - 1] = '\0';

	if (ofono_card_link_modem(ocd) == -1) {
		error("Couldn't link oFono card with modem: %s", card);
		goto fail;
	}

	if ((t = ofono_transport_new(d, profile, dbus_sender,
				card, ocd->modem_path)) == NULL) {
		error("Couldn't create new transport: %s", strerror(errno));
		goto fail;
	}

#if ENABLE_MSBC
	if (config.hfp.codecs.msbc &&
			profile == BA_TRANSPORT_PROFILE_HFP_AG &&
			ba_transport_get_codec(t) == HFP_CODEC_UNDEFINED)
		ofono_new_connection_request(t);
#endif

	/* initialize speaker and microphone volumes */
	ofono_call_volume_get_properties(t);

	g_hash_table_insert(ofono_card_data_map, ocd->card, ocd);
	ocd = NULL;

fail:
	if (a != NULL)
		ba_adapter_unref(a);
	if (d != NULL)
		ba_device_unref(d);
	if (t != NULL)
		ba_transport_unref(t);
	if (value != NULL)
		g_variant_unref(value);
	free(ocd);
}

/**
 * Get all oFono cards (phones). */
static int ofono_get_all_cards(void) {

	GDBusMessage *msg = NULL, *rep = NULL;
	GError *err = NULL;
	int ret = -1;

	msg = g_dbus_message_new_method_call(OFONO_SERVICE, "/",
			OFONO_IFACE_HF_AUDIO_MANAGER, "GetCards");

	if ((rep = g_dbus_connection_send_message_with_reply_sync(config.dbus, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, &err)) == NULL ||
			g_dbus_message_to_gerror(rep, &err))
		goto fail;

	const char *sender = g_dbus_message_get_sender(rep);
	GVariant *body = g_dbus_message_get_body(rep);

	GVariantIter *cards;
	GVariantIter *properties;
	const char *card;

	g_variant_get(body, "(a(oa{sv}))", &cards);
	while (g_variant_iter_next(cards, "(&oa{sv})", &card, &properties)) {
		ofono_card_add(sender, card, properties);
		g_variant_iter_free(properties);
	}

	g_variant_iter_free(cards);

	ret = 0;

fail:
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
	struct ofono_card_data *ocd;

	g_hash_table_iter_init(&iter, ofono_card_data_map);
	while (g_hash_table_iter_next(&iter, NULL, (void *)&ocd)) {

		debug("Removing oFono card: %s", ocd->card);

		struct ba_transport *t;
		if ((t = ofono_transport_lookup(ocd)) != NULL)
			ba_transport_destroy(t);

	}

	g_hash_table_remove_all(ofono_card_data_map);

}

static void ofono_agent_new_connection(GDBusMethodInvocation *inv, void *userdata) {
	(void)userdata;

	GDBusMessage *msg = g_dbus_method_invocation_get_message(inv);
	GVariant *params = g_dbus_method_invocation_get_parameters(inv);

	struct ba_transport *t = NULL;
	GError *err = NULL;
	const char *card;
	uint8_t codec;
	int fd;

	g_variant_get(params, "(&ohy)", &card, NULL, &codec);

	GUnixFDList *fd_list = g_dbus_message_get_unix_fd_list(msg);
	if ((fd = g_unix_fd_list_get(fd_list, 0, &err)) == -1) {
		error("Couldn't obtain oFono SCO link socket: %s", err->message);
		goto fail;
	}

	if ((t = ofono_transport_lookup_card(card)) == NULL) {
		error("Couldn't lookup transport: %s: %s", card, strerror(errno));
		goto fail;
	}

#if ENABLE_MSBC
	/* In AG mode, we obtain the codec when the device connects by
	 * performing a temporary acquisition. The response to that initial
	 * acquisition request is the only situation in which this function
	 * is called with the transport codec not yet set. */
	if (config.hfp.codecs.msbc &&
			t->profile == BA_TRANSPORT_PROFILE_HFP_AG &&
			ba_transport_get_codec(t) == HFP_CODEC_UNDEFINED) {

		/* Immediately release the SCO connection to save battery: we are only
		 * interested in the selected codec here. */
		if (fd != -1) {
			shutdown(fd, SHUT_RDWR);
			close(fd);
		}

		debug("Initialized oFono SCO link codec: %#x", codec);
		ba_transport_set_codec(t, codec);
		ba_transport_unref(t);

		g_dbus_method_invocation_return_value(inv, NULL);
		return;
	}
#endif

	/* For HF, oFono does not authorize after setting the voice option, so we
	 * may have to do it ourselves here. oFono always tries to set the
	 * BT_DEFER_SETUP option, but may not always succeed. So we must first
	 * check if the socket is in deferred setup state before authorizing. */
	if (t->profile == BA_TRANSPORT_PROFILE_HFP_HF) {
		/* If socket is not writable, it means that it is in the defer setup
		 * state, so it needs to be read to authorize the connection. */
		struct pollfd pfd = { fd, POLLOUT, 0 };
		uint8_t auth;
		if (poll(&pfd, 1, 0) == -1)
			goto fail;
		if (!(pfd.revents & POLLOUT) && read(fd, &auth, sizeof(auth)) == -1) {
			error("Couldn't authorize oFono SCO link: %s", strerror(errno));
			goto fail;
		}
	}

	ba_transport_stop(t);

	pthread_mutex_lock(&t->bt_fd_mtx);

	debug("New oFono SCO link (codec: %#x): %d", codec, fd);

	t->bt_fd = fd;
	t->mtu_read = t->mtu_write = hci_sco_get_mtu(fd, t->d->a);
	ba_transport_set_codec(t, codec);

	pthread_mutex_unlock(&t->bt_fd_mtx);

	ba_transport_pcm_state_set_idle(&t->sco.pcm_spk);
	ba_transport_pcm_state_set_idle(&t->sco.pcm_mic);
	ba_transport_start(t);

	g_dbus_method_invocation_return_value(inv, NULL);
	goto final;

fail:
	g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
		G_DBUS_ERROR_INVALID_ARGS, "Unable to get connection");
	if (fd != -1) {
		shutdown(fd, SHUT_RDWR);
		close(fd);
	}

final:
	if (t != NULL)
		ba_transport_unref(t);
	if (err != NULL)
		g_error_free(err);
}

/**
 * Callback for the Release method, called when oFono is properly shutdown. */
static void ofono_agent_release(GDBusMethodInvocation *inv, void *userdata) {
	(void)userdata;
	ofono_remove_all_cards();
	g_object_unref(inv);
}

/**
 * Register to the oFono service.
 *
 * @return On success this function returns 0. Otherwise -1 is returned. */
int ofono_register(void) {

	static const GDBusMethodCallDispatcher dispatchers[] = {
		{ .method = "NewConnection",
			.handler = ofono_agent_new_connection },
		{ .method = "Release",
			.handler = ofono_agent_release },
		{ 0 },
	};

	static const GDBusInterfaceSkeletonVTable vtable = {
		.dispatchers = dispatchers,
	};

	if (!config.profile.hfp_ofono)
		return 0;

	GDBusMessage *msg = NULL, *rep = NULL;
	GError *err = NULL;
	int ret = -1;

	debug("Registering oFono audio agent: %s", dbus_agent_object_path);

	if (dbus_hf_agent == NULL) {
		OrgOfonoHandsfreeAudioAgentSkeleton *ifs_hf_agent;
		if ((ifs_hf_agent = org_ofono_handsfree_audio_agent_skeleton_new(&vtable, NULL, NULL)) == NULL)
			goto fail;
		GDBusInterfaceSkeleton *ifs = G_DBUS_INTERFACE_SKELETON(ifs_hf_agent);
		if (!g_dbus_interface_skeleton_export(ifs, config.dbus, dbus_agent_object_path, &err)) {
			g_object_unref(ifs_hf_agent);
			goto fail;
		}
		dbus_hf_agent = ifs_hf_agent;
	}

	msg = g_dbus_message_new_method_call(OFONO_SERVICE, "/",
			OFONO_IFACE_HF_AUDIO_MANAGER, "Register");

	GVariantBuilder options;

	g_variant_builder_init(&options, G_VARIANT_TYPE("ay"));
	if (config.hfp.codecs.cvsd)
		g_variant_builder_add(&options, "y", OFONO_AUDIO_CODEC_CVSD);
#if ENABLE_MSBC
	if (config.hfp.codecs.msbc)
		g_variant_builder_add(&options, "y", OFONO_AUDIO_CODEC_MSBC);
#endif

	g_dbus_message_set_body(msg, g_variant_new("(oay)", dbus_agent_object_path, &options));
	g_variant_builder_clear(&options);

	if ((rep = g_dbus_connection_send_message_with_reply_sync(config.dbus, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, &err)) == NULL ||
			g_dbus_message_to_gerror(rep, &err))
		goto fail;

	ofono_get_all_cards();

	ret = 0;

fail:
	if (msg != NULL)
		g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);
	if (err != NULL) {
		warn("Couldn't register oFono: %s", err->message);
		g_error_free(err);
	}

	return ret;
}

/**
 * Callback for the CardAdded signal (emitted when phone is connected). */
static void ofono_signal_card_added(GDBusConnection *conn, const char *sender,
		const char *path, const char *interface, const char *signal, GVariant *params,
		void *userdata) {
	(void)conn;
	(void)path;
	(void)interface;
	(void)signal;
	(void)userdata;

	const char *card = NULL;
	GVariantIter *properties = NULL;

	g_variant_get(params, "(&oa{sv})", &card, &properties);
	debug("Signal: %s.%s(%s, ...)", interface, signal, card);

	ofono_card_add(sender, card, properties);

	g_variant_iter_free(properties);

}

/**
 * Callback for the CardRemoved signal (emitted when phone is disconnected). */
static void ofono_signal_card_removed(GDBusConnection *conn, const char *sender,
		const char *path, const char *interface, const char *signal, GVariant *params,
		void *userdata) {
	(void)conn;
	(void)sender;
	(void)path;
	(void)interface;
	(void)signal;
	(void)userdata;

	const char *card = NULL;
	g_variant_get(params, "(&o)", &card);
	debug("Signal: %s.%s(%s)", interface, signal, card);

	struct ba_transport *t;
	if ((t = ofono_transport_lookup_card(card)) != NULL)
		ba_transport_destroy(t);

	g_hash_table_remove(ofono_card_data_map, card);

}

/**
 * Callback for the PropertyChanged signal on the CallVolume interface. */
static void ofono_signal_volume_changed(GDBusConnection *conn, const char *sender,
		const char *modem_path, const char *interface, const char *signal, GVariant *params,
		void *userdata) {
	(void)conn;
	(void)sender;
	(void)interface;
	(void)signal;
	(void)userdata;

	struct ba_transport *t;
	if ((t = ofono_transport_lookup_modem(modem_path)) == NULL) {
		error("Couldn't lookup transport: %s: %s", modem_path, strerror(errno));
		return;
	}

	const char *property;
	GVariant *value;

	g_variant_get(params, "(&sv)", &property, &value);
	debug("Signal: %s.%s(%s, ...)", interface, signal, property);

	unsigned int mask = ofono_call_volume_property_sync(t, property, value);
	if (mask & OFONO_CALL_VOLUME_SPEAKER)
		bluealsa_dbus_pcm_update(&t->sco.pcm_spk, BA_DBUS_PCM_UPDATE_VOLUME);
	if (mask & OFONO_CALL_VOLUME_MICROPHONE)
		bluealsa_dbus_pcm_update(&t->sco.pcm_mic, BA_DBUS_PCM_UPDATE_VOLUME);

	g_variant_unref(value);
	ba_transport_unref(t);

}

/**
 * Monitor oFono service appearance. */
static void ofono_appeared(GDBusConnection *conn, const char *name,
		const char *owner, void *userdata) {
	(void)conn;
	(void)name;
	(void)owner;
	(void)userdata;
	ofono_register();
}

/**
 * Monitor oFono service disappearance.
 *
 * When oFono is properly shutdown, we are notified through the Release()
 * method. Here, we get the opportunity to perform some cleanup if oFono
 * was killed. */
static void ofono_disappeared(GDBusConnection *conn, const char *name,
		void *userdata) {
	(void)conn;
	(void)name;
	(void)userdata;
	ofono_remove_all_cards();
}

/**
 * Initialize integration with oFono service.
 *
 * @return On success this function returns 0. Otherwise -1 is returned. */
int ofono_init(void) {

	if (!config.profile.hfp_ofono)
		return 0;

	if (ofono_card_data_map == NULL)
		ofono_card_data_map = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, free);

	g_dbus_connection_signal_subscribe(config.dbus, OFONO_SERVICE,
			OFONO_IFACE_HF_AUDIO_MANAGER, "CardAdded", NULL, NULL,
			G_DBUS_SIGNAL_FLAGS_NONE, ofono_signal_card_added, NULL, NULL);
	g_dbus_connection_signal_subscribe(config.dbus, OFONO_SERVICE,
			OFONO_IFACE_HF_AUDIO_MANAGER, "CardRemoved", NULL, NULL,
			G_DBUS_SIGNAL_FLAGS_NONE, ofono_signal_card_removed, NULL, NULL);

	g_dbus_connection_signal_subscribe(config.dbus, OFONO_SERVICE,
			OFONO_IFACE_CALL_VOLUME, "PropertyChanged", NULL, NULL,
			G_DBUS_SIGNAL_FLAGS_NONE, ofono_signal_volume_changed, NULL, NULL);

	g_bus_watch_name_on_connection(config.dbus, OFONO_SERVICE,
			G_BUS_NAME_WATCHER_FLAGS_NONE, ofono_appeared, ofono_disappeared,
			NULL, NULL);

	return 0;
}

/**
 * Check whether oFono service is running. */
bool ofono_detect_service(void) {

	GDBusMessage *msg = NULL, *rep = NULL;
	bool status = true;

	debug("Checking oFono service presence");

	msg = g_dbus_message_new_method_call(OFONO_SERVICE, "/",
			OFONO_IFACE_MANAGER, "GetModems");
	if ((rep = g_dbus_connection_send_message_with_reply_sync(config.dbus, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, NULL)) == NULL ||
			g_dbus_message_get_message_type(rep) == G_DBUS_MESSAGE_TYPE_ERROR)
		status = false;

	if (msg != NULL)
		g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);

	return status;
}

/**
 * Update oFono call volume properties. */
int ofono_call_volume_update(struct ba_transport *t) {

	struct ba_transport_pcm *spk = &t->sco.pcm_spk;
	struct ba_transport_pcm *mic = &t->sco.pcm_mic;
	int ret = 0;

	const struct {
		const char *name;
		GVariant *value;
	} props[] = {
		{ "Muted",
			g_variant_new_boolean(mic->volume[0].scale == 0) },
		{ "SpeakerVolume",
			g_variant_new_byte(MIN(100,
					ba_transport_pcm_volume_level_to_range(spk->volume[0].level, 106))) },
		{ "MicrophoneVolume",
			g_variant_new_byte(MIN(100,
					ba_transport_pcm_volume_level_to_range(mic->volume[0].level, 106))) },
	};

	for (size_t i = 0; i < ARRAYSIZE(props); i++) {
		GError *err = NULL;
		if (ofono_call_volume_set_property(t, props[i].name, props[i].value, &err) == -1) {
			error("Couldn't set oFono call volume: %s: %s", props[i].name, err->message);
			g_error_free(err);
			ret = -1;
		}
	}

	return ret;
}
