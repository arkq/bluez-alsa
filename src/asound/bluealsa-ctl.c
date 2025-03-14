/*
 * BlueALSA - asound/bluealsa-ctl.c
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/param.h>
#include <unistd.h>

#include <alsa/asoundlib.h>
#include <alsa/control_external.h>
#include <bluetooth/bluetooth.h>
#include <dbus/dbus.h>

#include "shared/dbus-client.h"
#include "shared/dbus-client-pcm.h"
#include "shared/defs.h"

#define DELAY_SYNC_STEP 250
#define DELAY_SYNC_MIN_VALUE (INT16_MIN / DELAY_SYNC_STEP * DELAY_SYNC_STEP)
#define DELAY_SYNC_MAX_VALUE (INT16_MAX / DELAY_SYNC_STEP * DELAY_SYNC_STEP)
#define DELAY_SYNC_NUM_VALUES (1 + (DELAY_SYNC_MAX_VALUE - DELAY_SYNC_MIN_VALUE) / DELAY_SYNC_STEP)

/**
 * Control element type.
 *
 * Note: The order of enum values is important - it
 *       determines control elements ordering. */
enum ctl_elem_type {
	CTL_ELEM_TYPE_SWITCH,
	CTL_ELEM_TYPE_VOLUME,
	CTL_ELEM_TYPE_VOLUME_MODE,
	CTL_ELEM_TYPE_CODEC,
	CTL_ELEM_TYPE_DELAY_SYNC,
	CTL_ELEM_TYPE_BATTERY,
};

/**
 * Control element. */
struct ctl_elem {
	enum ctl_elem_type type;
	struct bt_dev *dev;
	struct ba_pcm *pcm;
	/* element ID exposed by ALSA */
	int numid;
	char name[44 /* internal ALSA constraint */];
	unsigned int index;
	/* codec list for codec control element */
	struct ba_pcm_codecs codecs;
	/* if true, element is a playback control */
	bool playback;
	/* For single device mode, if true then the associated profile is connected.
	 * If false, the element value is zero, and writes are ignored. */
	bool active;
};

struct ctl_elem_update {
	/* PCM associated with the element being updated. This pointer shall not
	 * be dereferenced, because it might point to already freed memory region. */
	const struct ba_pcm *pcm;
	/* the ID of the element */
	int numid;
	/* the name of the element being updated */
	char name[sizeof(((struct ctl_elem *)0)->name)];
	/* index of the element being updated */
	unsigned int index;
	int event_mask;
};

#define BT_DEV_MASK_NONE   (0)
#define BT_DEV_MASK_ADD    (1 << 0)
#define BT_DEV_MASK_REMOVE (1 << 1)
#define BT_DEV_MASK_UPDATE (1 << 2)

struct bt_dev {
	char device_path[sizeof(((struct ba_pcm *)0)->device_path)];
	char rfcomm_path[sizeof(((struct ba_pcm *)0)->device_path)];
	char name[sizeof(((struct ctl_elem *)0)->name)];
	int battery_level;
	int mask;
};

struct bluealsa_ctl {
	snd_ctl_ext_t ext;

	/* D-Bus connection context */
	struct ba_dbus_ctx dbus_ctx;

	/* list of BT devices */
	struct bt_dev **dev_list;
	size_t dev_list_size;

	/* list of all BlueALSA PCMs */
	struct ba_pcm **pcm_list;
	size_t pcm_list_size;

	/* list of ALSA control elements */
	struct ctl_elem *elem_list;
	size_t elem_list_size;

	/* list of control element update events */
	struct ctl_elem_update *elem_update_list;
	size_t elem_update_list_size;
	size_t elem_update_event_i;

	/* Event pipe. Allows us to trigger events internally and to generate a
	 * POLLERR event by closing the read end then polling the write end.
	 * Many applications (including alsamixer) interpret POLLERR as
	 * indicating the mixer device has been disconnected. */
	int pipefd[2];

	/* if true, show codec control */
	bool show_codec;
	/* if true, show volume mode control */
	bool show_vol_mode;
	/* if true, show client delay sync control */
	bool show_delay_sync;
	/* if true, show battery level indicator */
	bool show_battery;
	/* if true, append BT transport type to element names */
	bool show_bt_transport;
	/* if true, this mixer is for a single Bluetooth device */
	bool single_device;
	/* if true, this mixer adds/removes controls dynamically */
	bool dynamic;

};

static const char *soft_volume_names[] = {
	"pass-through",
	"software",
};

static int str2bdaddr(const char *str, bdaddr_t *ba) {

	unsigned int x[6];
	if (sscanf(str, "%x:%x:%x:%x:%x:%x",
				&x[5], &x[4], &x[3], &x[2], &x[1], &x[0]) != 6)
		return -1;

	for (size_t i = 0; i < 6; i++)
		ba->b[i] = x[i];

	return 0;
}

static int bluealsa_bt_dev_cmp(const void *p1, const void *p2) {
	const struct bt_dev *d1 = *(const struct bt_dev **)p1;
	const struct bt_dev *d2 = *(const struct bt_dev **)p2;
	return strcmp(d1->device_path, d2->device_path);
}

static int bluealsa_elem_cmp(const void *p1, const void *p2) {

	const struct ctl_elem *e1 = (const struct ctl_elem *)p1;
	const struct ctl_elem *e2 = (const struct ctl_elem *)p2;
	int rv;

	/* Sort elements by device names. In case were names
	 * are the same sort by device addresses. */
	if ((rv = bacmp(&e1->pcm->addr, &e2->pcm->addr)) != 0) {
		const int dev_rv = strcmp(e1->dev->name, e2->dev->name);
		return dev_rv != 0 ? dev_rv : rv;
	}

	/* Within a single device order elements by:
	 *  - PCM transport type
	 *  - playback/capture (if applicable)
	 *  - element type
	 * */
	if ((rv = e1->pcm->transport - e2->pcm->transport))
		return rv;
	if (!(e1->type == CTL_ELEM_TYPE_CODEC ||
				e1->type == CTL_ELEM_TYPE_BATTERY ||
				e2->type == CTL_ELEM_TYPE_CODEC ||
				e2->type == CTL_ELEM_TYPE_BATTERY))
		if ((rv = e1->playback - e2->playback) != 0)
			return -rv;
	if ((rv = e1->type - e2->type) != 0)
		return rv;
	return 0;
}

static DBusMessage *bluealsa_dbus_get_property(DBusConnection *conn,
		const char *service, const char *path, const char *interface,
		const char *property, DBusError *error) {

	DBusMessage *msg;
	if ((msg = dbus_message_new_method_call(service, path,
					DBUS_INTERFACE_PROPERTIES, "Get")) == NULL)
		return NULL;

	DBusMessage *rep = NULL;

	if (!dbus_message_append_args(msg,
			DBUS_TYPE_STRING, &interface,
			DBUS_TYPE_STRING, &property,
			DBUS_TYPE_INVALID))
		goto fail;

	rep = dbus_connection_send_with_reply_and_block(conn, msg,
			DBUS_TIMEOUT_USE_DEFAULT, error);

fail:
	dbus_message_unref(msg);
	return rep;
}

/**
 * Get BT device ID number.
 *
 * @param ctl The BlueALSA controller context.
 * @param pcm BlueALSA PCM structure.
 * @return The device ID number, or -1 upon error. */
static int bluealsa_dev_get_id(struct bluealsa_ctl *ctl, const struct ba_pcm *pcm) {
	for (size_t i = 0; i < ctl->dev_list_size; i++)
		if (strcmp(ctl->dev_list[i]->device_path, pcm->device_path) == 0)
			return i + 1;
	return -1;
}

static int bluealsa_dev_fetch_name(struct bluealsa_ctl *ctl, struct bt_dev *dev) {

	DBusMessage *rep;
	DBusError err = DBUS_ERROR_INIT;
	if ((rep = bluealsa_dbus_get_property(ctl->dbus_ctx.conn, "org.bluez",
					dev->device_path, "org.bluez.Device1", "Alias", &err)) == NULL) {
		SNDERR("Couldn't get device name: %s", err.message);
		dbus_error_free(&err);
		return -1;
	}

	DBusMessageIter iter;
	DBusMessageIter iter_val;

	dbus_message_iter_init(rep, &iter);
	dbus_message_iter_recurse(&iter, &iter_val);

	const char *name;
	dbus_message_iter_get_basic(&iter_val, &name);
	*stpncpy(dev->name, name, sizeof(dev->name) - 1) = '\0';

	dbus_message_unref(rep);
	return 0;
}

static int bluealsa_dev_fetch_battery(struct bluealsa_ctl *ctl, struct bt_dev *dev) {

	DBusMessage *rep;
	DBusError err = DBUS_ERROR_INIT;
	if ((rep = bluealsa_dbus_get_property(ctl->dbus_ctx.conn, ctl->dbus_ctx.ba_service,
					dev->rfcomm_path, BLUEALSA_INTERFACE_RFCOMM, "Battery", &err)) == NULL) {
		SNDERR("Couldn't get device battery status: %s", err.message);
		dbus_error_free(&err);
		return -1;
	}

	DBusMessageIter iter;
	DBusMessageIter iter_val;

	dbus_message_iter_init(rep, &iter);
	dbus_message_iter_recurse(&iter, &iter_val);

	signed char level;
	dbus_message_iter_get_basic(&iter_val, &level);
	dev->battery_level = level;

	dbus_message_unref(rep);
	return level;
}

static int bluealsa_pcm_fetch_codecs(struct bluealsa_ctl *ctl, struct ba_pcm *pcm,
		struct ba_pcm_codecs *codecs) {

	codecs->codecs = NULL;
	codecs->codecs_len = 0;

	/* Note: We are not checking for errors when calling this function. Failure
	 *       most likely means that the PCM for which we are fetching codecs is
	 *       already removed by the BlueALSA server. It will happen when server
	 *       removes PCM but ALSA control plug-in was not yet able to process
	 *       elem remove event. */
	ba_dbus_pcm_codecs_get(&ctl->dbus_ctx, pcm->pcm_path, codecs, NULL);

	/* If the list of codecs could not be fetched, return currently
	 * selected codec as the only one. This will at least allow the
	 * user to see the currently selected codec. */
	if (codecs->codecs_len == 0) {
		if ((codecs->codecs = malloc(sizeof(*codecs->codecs))) == NULL)
			return -1;
		memcpy(codecs->codecs, &pcm->codec, sizeof(*codecs->codecs));
		codecs->codecs_len = 1;
	}

	return codecs->codecs_len;
}

/**
 * Get BT device structure.
 *
 * @param ctl The BlueALSA controller context.
 * @param pcm BlueALSA PCM structure.
 * @return The BT device, or NULL upon error. */
static struct bt_dev *bluealsa_dev_get(struct bluealsa_ctl *ctl, const struct ba_pcm *pcm) {

	for (size_t i = 0; i < ctl->dev_list_size; i++)
		if (strcmp(ctl->dev_list[i]->device_path, pcm->device_path) == 0)
			return ctl->dev_list[i];

	/* If device is not cached yet, fetch data from
	 * the BlueZ via the B-Bus interface. */

	struct bt_dev **dev_list = ctl->dev_list;
	size_t size = ctl->dev_list_size;
	if ((dev_list = realloc(dev_list, (size + 1) * sizeof(*dev_list))) == NULL)
		return NULL;
	ctl->dev_list = dev_list;

	struct bt_dev *dev;
	if ((dev_list[size] = dev = malloc(sizeof(*dev))) == NULL)
		return NULL;
	ctl->dev_list_size++;

	strcpy(dev->device_path, pcm->device_path);
	sprintf(dev->rfcomm_path, "/org/bluealsa%.64s/rfcomm", &dev->device_path[10]);
	sprintf(dev->name, "%.2X:%.2X:%.2X:%.2X:%.2X:%.2X",
			pcm->addr.b[5], pcm->addr.b[4], pcm->addr.b[3],
			pcm->addr.b[2], pcm->addr.b[1], pcm->addr.b[0]);
	dev->battery_level = -1;

	/* Sort device list by an object path, so the bluealsa_dev_get_id() will
	 * return consistent IDs ordering in case of name duplications. */
	qsort(dev_list, ctl->dev_list_size, sizeof(*dev_list), bluealsa_bt_dev_cmp);

	bluealsa_dev_fetch_name(ctl, dev);
	return dev;
}

static ssize_t bluealsa_pipefd_ping(struct bluealsa_ctl *ctl) {
	char ping = 1;
	return write(ctl->pipefd[1], &ping, sizeof(ping));
}

static ssize_t bluealsa_pipefd_flush(struct bluealsa_ctl *ctl) {
	char buffer[16];
	return read(ctl->pipefd[0], buffer, sizeof(buffer));
}

static int bluealsa_elem_update_list_add(struct bluealsa_ctl *ctl,
		const struct ctl_elem *elem, unsigned int mask) {

	struct ctl_elem_update *tmp = ctl->elem_update_list;
	if ((tmp = realloc(tmp, (ctl->elem_update_list_size + 1) * sizeof(*tmp))) == NULL)
		return -1;

	tmp[ctl->elem_update_list_size].numid = elem->numid;
	tmp[ctl->elem_update_list_size].pcm = elem->pcm;
	tmp[ctl->elem_update_list_size].event_mask = mask;
	const size_t len = sizeof(tmp[ctl->elem_update_list_size].name) - 1;
	memcpy(tmp[ctl->elem_update_list_size].name, elem->name, len);
	tmp[ctl->elem_update_list_size].name[len] = '\0';
	tmp[ctl->elem_update_list_size].index = elem->index;

	ctl->elem_update_list = tmp;
	ctl->elem_update_list_size++;
	return 0;
}

#define bluealsa_event_elem_added(ctl, elem) \
	bluealsa_elem_update_list_add(ctl, elem, SND_CTL_EVENT_MASK_ADD)
#define bluealsa_event_elem_removed(ctl, elem) \
	bluealsa_elem_update_list_add(ctl, elem, SND_CTL_EVENT_MASK_REMOVE)
#define bluealsa_event_elem_updated(ctl, elem) \
	bluealsa_elem_update_list_add(ctl, elem, SND_CTL_EVENT_MASK_VALUE)

/**
 * Add new PCM to the list of known PCMs. */
static int bluealsa_pcm_add(struct bluealsa_ctl *ctl, const struct ba_pcm *pcm) {
	struct ba_pcm **list = ctl->pcm_list;
	const size_t list_size = ctl->pcm_list_size;
	if ((list = realloc(list, (list_size + 1) * sizeof(*list))) == NULL)
		return -1;
	ctl->pcm_list = list;
	if ((list[list_size] = malloc(sizeof(*list[list_size]))) == NULL)
		return -1;
	memcpy(list[list_size], pcm, sizeof(*list[list_size]));
	ctl->pcm_list_size++;
	return 0;
}

/**
 * Remove PCM from the list of known PCMs. */
static int bluealsa_pcm_remove(struct bluealsa_ctl *ctl, const char *path) {
	for (size_t i = 0; i < ctl->pcm_list_size; i++)
		if (strcmp(ctl->pcm_list[i]->pcm_path, path) == 0) {

			/* clear all pending events associated with removed PCM */
			for (size_t ii = 0; ii < ctl->elem_update_list_size; ii++)
				if (ctl->elem_update_list[ii].pcm == ctl->pcm_list[i])
					ctl->elem_update_list[ii].event_mask = 0;

			/* remove PCM from the list */
			free(ctl->pcm_list[i]);
			ctl->pcm_list[i] = ctl->pcm_list[--ctl->pcm_list_size];

		}
	return 0;
}

static int bluealsa_pcm_activate(struct bluealsa_ctl *ctl, const struct ba_pcm *pcm) {
	for (size_t i = 0; i < ctl->pcm_list_size; i++)
		if (strcmp(ctl->pcm_list[i]->pcm_path, pcm->pcm_path) == 0) {

			/* update potentially stalled PCM data */
			memcpy(ctl->pcm_list[i], pcm, sizeof(*ctl->pcm_list[i]));

			/* activate associated elements */
			for (size_t el = 0; el < ctl->elem_list_size; el++)
				if (ctl->elem_list[el].pcm == ctl->pcm_list[i]) {
					ctl->elem_list[el].active = true;
					bluealsa_event_elem_updated(ctl, &ctl->elem_list[el]);
				}

			break;
		}
	return 0;
}

static int bluealsa_pcm_deactivate(struct bluealsa_ctl *ctl, const char *path) {
	for (size_t i = 0; i < ctl->elem_list_size; i++)
		if (strcmp(ctl->elem_list[i].pcm->pcm_path, path) == 0) {
			ctl->elem_list[i].active = false;
			bluealsa_event_elem_updated(ctl, &ctl->elem_list[i]);
		}
	return 0;
}

static const char *transport2str(unsigned int transport) {
	switch (transport) {
	case BA_PCM_TRANSPORT_A2DP_SOURCE:
		return "-SRC";
	case BA_PCM_TRANSPORT_A2DP_SINK:
		return "-SNK";
	case BA_PCM_TRANSPORT_HFP_AG:
		return "-HFP-AG";
	case BA_PCM_TRANSPORT_HFP_HF:
		return "-HFP-HF";
	case BA_PCM_TRANSPORT_HSP_AG:
		return "-HSP-AG";
	case BA_PCM_TRANSPORT_HSP_HS:
		return "-HSP-HS";
	default:
		return "";
	}
}

static int parse_extended(const char *extended,
		bool *show_codec, bool *show_vol_mode, bool *show_delay_sync, bool *show_battery) {

	bool codec = false, vol_mode = false, sync = false, battery = false;
	int ret = 0;

	switch (snd_config_get_bool_ascii(extended)) {
	case 0:
		break;
	case 1:
		codec = true;
		vol_mode = true;
		sync = true;
		battery = true;
		break;
	default: {
		char *next, *ptr = NULL;
		char *buffer = strdupa(extended);
		for (next = strtok_r(buffer, ":", &ptr);
				next != NULL;
				next = strtok_r(NULL, ":", &ptr)) {
			if (strcasecmp(next, "codec") == 0)
				codec = true;
			else if (strcasecmp(next, "mode") == 0)
				vol_mode = true;
			else if (strcasecmp(next, "sync") == 0)
				sync = true;
			else if (strcasecmp(next, "battery") == 0)
				battery = true;
			else {
				ret = -1;
				break;
			}
		}
	}}

	if (ret != -1) {
		*show_codec = codec;
		*show_vol_mode = vol_mode;
		*show_delay_sync = sync;
		*show_battery = battery;
	}

	return ret;
}

/**
 * Update element name based on given string and PCM type.
 *
 * @param ctl The BlueALSA controller context.
 * @param elem An address to the element structure.
 * @param name A string which should be used as a base for the element name. May
 *   be NULL if no base prefix is required.
 * @param with_device_id If true, Bluetooth device ID number will be attached
 *   to the element name in order to prevent duplications. */
static void bluealsa_elem_set_name(struct bluealsa_ctl *ctl, struct ctl_elem *elem,
		const char *name, bool with_device_id) {

	const char *transport = "";
	if (ctl->show_bt_transport)
		transport = transport2str(elem->pcm->transport);

	if (name != NULL) {
		/* multi-device mixer - include device alias in control names */

		const size_t name_len = strlen(name);
		/* max name length with reserved space for ALSA suffix */
		int len = sizeof(elem->name) - 16 - 1;
		char no[16] = "";

		if (with_device_id) {
			sprintf(no, " #%u", bluealsa_dev_get_id(ctl, elem->pcm));
			len -= strlen(no);
		}

		/* get the longest possible element label */
		size_t label_max_len = sizeof(" A2DP") - 1;
		if (ctl->show_bt_transport)
			label_max_len = sizeof(" SCO-HFP-AG") - 1;
		if (ctl->show_vol_mode)
			label_max_len += sizeof(" Mode") - 1;
		else if (ctl->show_delay_sync)
			label_max_len += sizeof(" Sync") - 1;
		if (ctl->show_battery)
			label_max_len = MAX(label_max_len, sizeof(" | Battery") - 1);

		/* Reserve space for the longest element type description. This applies
		 * to all elements so the shortened device name will be consistent. */
		len = MIN(len - label_max_len, name_len);
		while (isspace(name[len - 1]))
			len--;

		if (elem->type == CTL_ELEM_TYPE_BATTERY) {
			sprintf(elem->name, "%.*s%s | Battery", len, name, no);
		}
		else {
			/* avoid name duplication by adding profile suffixes */
			switch (elem->pcm->transport) {
			case BA_PCM_TRANSPORT_A2DP_SOURCE:
			case BA_PCM_TRANSPORT_A2DP_SINK:
				sprintf(elem->name, "%.*s%s A2DP%s", len, name, no, transport);
				break;
			case BA_PCM_TRANSPORT_HFP_AG:
			case BA_PCM_TRANSPORT_HFP_HF:
			case BA_PCM_TRANSPORT_HSP_AG:
			case BA_PCM_TRANSPORT_HSP_HS:
				sprintf(elem->name, "%.*s%s SCO%s", len, name, no, transport);
				break;
			}
		}
	}
	else {
		/* single-device mixer - constant control names */
		if (elem->type == CTL_ELEM_TYPE_BATTERY)
			strcpy(elem->name, "Battery");
		else
			switch (elem->pcm->transport) {
			case BA_PCM_TRANSPORT_A2DP_SOURCE:
			case BA_PCM_TRANSPORT_A2DP_SINK:
				sprintf(elem->name, "A2DP%s", transport);
				break;
			case BA_PCM_TRANSPORT_HFP_AG:
			case BA_PCM_TRANSPORT_HFP_HF:
			case BA_PCM_TRANSPORT_HSP_AG:
			case BA_PCM_TRANSPORT_HSP_HS:
				sprintf(elem->name, "SCO%s", transport);
				break;
			}
	}

	if (elem->type == CTL_ELEM_TYPE_CODEC)
		strcat(elem->name, " Codec");

	if (elem->type == CTL_ELEM_TYPE_VOLUME_MODE)
		strcat(elem->name, " Mode");

	if (elem->type == CTL_ELEM_TYPE_DELAY_SYNC)
		strcat(elem->name, " Sync");

	/* ALSA library determines the element type by checking it's
	 * name suffix. This feature is not well documented, though.
	 * A codec control is 'Global' (i.e. neither 'Playback' nor
	 * 'Capture') so we omit the suffix in that case. */
	if (elem->type != CTL_ELEM_TYPE_CODEC)
		strcat(elem->name, elem->playback ? " Playback" : " Capture");

	switch (elem->type) {
	case CTL_ELEM_TYPE_SWITCH:
		strcat(elem->name, " Switch");
		break;
	case CTL_ELEM_TYPE_BATTERY:
	case CTL_ELEM_TYPE_VOLUME:
		strcat(elem->name, " Volume");
		break;
	case CTL_ELEM_TYPE_CODEC:
	case CTL_ELEM_TYPE_VOLUME_MODE:
	case CTL_ELEM_TYPE_DELAY_SYNC:
		strcat(elem->name, " Enum");
		break;
	}

}

/**
 * Create control elements for a given PCM.
 *
 * @param ctl The BlueALSA controller context.
 * @param elem_list An address to the array of element structures. This array
 *   must have sufficient space for new elements which includes volume element,
 *   switch element and optional battery indicator element.
 * @param dev The BT device associated with created elements.
 * @param pcm The BlueALSA PCM associated with created elements.
 * @param codecs The list of available PCM codecs. If not empty, additional
 *   control element for codec selection will be created. The ownership of
 *   the codec list structure is transferred to associated control element.
 * @param add_battery_elem If true, add battery level indicator element.
 * @return The number of elements added. */
static size_t bluealsa_elem_list_add_pcm_elems(struct bluealsa_ctl *ctl,
		struct ctl_elem *elem_list, struct bt_dev *dev, struct ba_pcm *pcm,
		struct ba_pcm_codecs *codecs, bool add_battery_elem) {

	const char *name = ctl->single_device ? NULL : dev->name;
	const bool playback = pcm->mode == BA_PCM_MODE_SINK;
	size_t n = 0;

	elem_list[n].type = CTL_ELEM_TYPE_VOLUME;
	elem_list[n].dev = dev;
	elem_list[n].pcm = pcm;
	elem_list[n].playback = playback;
	elem_list[n].active = true;
	bluealsa_elem_set_name(ctl, &elem_list[n], name, false);
	elem_list[n].index = 0;

	n++;

	elem_list[n].type = CTL_ELEM_TYPE_SWITCH;
	elem_list[n].dev = dev;
	elem_list[n].pcm = pcm;
	elem_list[n].playback = playback;
	elem_list[n].active = true;
	bluealsa_elem_set_name(ctl, &elem_list[n], name, false);
	elem_list[n].index = 0;

	n++;

	/* add special "codec" element */
	if (codecs->codecs_len > 0) {
		elem_list[n].type = CTL_ELEM_TYPE_CODEC;
		elem_list[n].dev = dev;
		elem_list[n].pcm = pcm;
		elem_list[n].playback = playback;
		elem_list[n].active = true;
		memcpy(&elem_list[n].codecs, codecs, sizeof(elem_list[n].codecs));
		bluealsa_elem_set_name(ctl, &elem_list[n], name, false);
		elem_list[n].index = 0;

		n++;
	}

	/* add special "volume mode" element */
	if (ctl->show_vol_mode) {
		elem_list[n].type = CTL_ELEM_TYPE_VOLUME_MODE;
		elem_list[n].dev = dev;
		elem_list[n].pcm = pcm;
		elem_list[n].playback = playback;
		elem_list[n].active = true;
		bluealsa_elem_set_name(ctl, &elem_list[n], name, false);

		/* ALSA library permits only one enumeration type control for
		 * each simple control id. So we use different index numbers
		 * for capture and playback to get different ids. */
		elem_list[n].index = playback ? 0 : 1;

		n++;
	}

	/* add special client delay "sync" element */
	if (ctl->show_delay_sync) {
		elem_list[n].type = CTL_ELEM_TYPE_DELAY_SYNC;
		elem_list[n].dev = dev;
		elem_list[n].pcm = pcm;
		elem_list[n].playback = playback;
		elem_list[n].active = true;
		bluealsa_elem_set_name(ctl, &elem_list[n], name, false);

		/* ALSA library permits only one enumeration type control for
		 * each simple control id. So we use different index numbers
		 * for capture and playback to get different ids. */
		elem_list[n].index = playback ? 0 : 1;

		n++;
	}

	/* add special battery level indicator element */
	if (add_battery_elem &&
			dev->battery_level != -1 &&
			/* There has to be attached some PCM to an element structure. Since
			 * battery level is set only when SCO profile is connected (battery
			 * requires RFCOMM), for simplicity and convenience, we will bind
			 * battery element with SCO sink PCM. */
			pcm->transport & BA_PCM_TRANSPORT_MASK_SCO &&
			pcm->mode == BA_PCM_MODE_SINK) {
		elem_list[n].type = CTL_ELEM_TYPE_BATTERY;
		elem_list[n].dev = dev;
		elem_list[n].pcm = pcm;
		elem_list[n].playback = true;
		elem_list[n].active = true;
		bluealsa_elem_set_name(ctl, &elem_list[n], name, false);
		elem_list[n].index = 0;

		n++;
	}

	return n;
}

static bool elem_list_dev_has_battery_elem(const struct ctl_elem *elem_list,
		size_t elem_list_size, const struct bt_dev *dev) {
	for (size_t i = 0; i < elem_list_size; i++)
		if (elem_list[i].type == CTL_ELEM_TYPE_BATTERY &&
				elem_list[i].dev == dev)
			return true;
	return false;
}

static int bluealsa_create_elem_list(struct bluealsa_ctl *ctl) {

	size_t count = 0;
	struct ctl_elem *elem_list = ctl->elem_list;

	if (ctl->pcm_list_size > 0) {

		for (size_t i = 0; i < ctl->pcm_list_size; i++) {
			/* Every stream has two controls associated to
			 * itself - volume adjustment and mute switch. */
			count += 2;
			/* It is possible, that BT device battery level will be exposed via
			 * RFCOMM interface, so in order to account for a special "battery"
			 * element we have to increment our element counter by one. */
			if (ctl->show_battery)
				count += 1;
			/* If extended controls are enabled, we need additional elements. */
			if (ctl->show_codec)
				count += 1;
			if (ctl->show_vol_mode)
				count += 1;
			if (ctl->show_delay_sync)
				count += 1;
		}

		if ((elem_list = realloc(elem_list, count * sizeof(*elem_list))) == NULL)
			return -1;

	}

	/* Clear device mask, so we can distinguish currently used and unused (old)
	 * device entries - we are not invalidating device list after PCM remove. */
	for (size_t i = 0; i < ctl->dev_list_size; i++)
		ctl->dev_list[i]->mask = BT_DEV_MASK_NONE;

	count = 0;

	/* Construct control elements based on available PCMs. */
	for (size_t i = 0; i < ctl->pcm_list_size; i++) {

		struct ba_pcm *pcm = ctl->pcm_list[i];
		struct bt_dev *dev = bluealsa_dev_get(ctl, pcm);
		struct ba_pcm_codecs codecs = { 0 };
		bool add_battery_elem = false;

		/* If Bluetooth transport is bi-directional it must have the same codec
		 * for both sink and source. In case of such profiles we will only add
		 * the codec control element for the main stream direction. */
		if (ctl->show_codec && (
					BA_PCM_A2DP_MAIN_CHANNEL(pcm) ||
					BA_PCM_SCO_SPEAKER_CHANNEL(pcm)))
			bluealsa_pcm_fetch_codecs(ctl, pcm, &codecs);

		if (ctl->show_battery &&
				!elem_list_dev_has_battery_elem(elem_list, count, dev)) {
			bluealsa_dev_fetch_battery(ctl, dev);
			add_battery_elem = true;
		}

		count += bluealsa_elem_list_add_pcm_elems(ctl, &elem_list[count],
				dev, pcm, &codecs, add_battery_elem);

	}

	if (count > 0)
		/* Sort control elements according to our sorting rules. */
		qsort(elem_list, count, sizeof(*elem_list), bluealsa_elem_cmp);

	/* Detect element name duplicates and annotate them with the
	 * consecutive device ID number - make ALSA library happy. */
	if (!ctl->single_device)
		for (size_t i = 0; i < count; i++) {

			bool duplicated = false;
			for (size_t ii = i + 1; ii < count; ii++)
				if (elem_list[i].dev != elem_list[ii].dev &&
						strcmp(elem_list[i].name, elem_list[ii].name) == 0) {
					bluealsa_elem_set_name(ctl, &elem_list[ii], elem_list[ii].dev->name, true);
					duplicated = true;
				}

			if (duplicated)
				bluealsa_elem_set_name(ctl, &elem_list[i], elem_list[i].dev->name, true);

		}

	/* Annotate elements with ALSA fake ID (see ALSA lib snd_ctl_ext_elem_list()
	 * function for reference). These IDs will not be used by the ALSA lib when
	 * the elem_list callback is called. However, we need them to be consistent
	 * with ALSA internal fake IDs, because we will use them when creating new
	 * elements by SND_CTL_EVENT_MASK_ADD events. Otherwise, these elements will
	 * not behave properly. */
	for (size_t i = 0; i < count; i++)
		elem_list[i].numid = i + 1;

	ctl->elem_list = elem_list;
	ctl->elem_list_size = count;

	return count;
}

static void bluealsa_free_elem_list(struct bluealsa_ctl *ctl) {
	for (size_t i = 0; i < ctl->elem_list_size; i++)
		if (ctl->elem_list[i].type == CTL_ELEM_TYPE_CODEC)
			ba_dbus_pcm_codecs_free(&ctl->elem_list[i].codecs);
}

static void bluealsa_close(snd_ctl_ext_t *ext) {

	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;

	ba_dbus_connection_ctx_free(&ctl->dbus_ctx);
	bluealsa_free_elem_list(ctl);

	if (ctl->pipefd[0] != -1)
		close(ctl->pipefd[0]);
	if (ctl->pipefd[1] != -1)
		close(ctl->pipefd[1]);

	for (size_t i = 0; i < ctl->dev_list_size; i++)
		free(ctl->dev_list[i]);
	for (size_t i = 0; i < ctl->pcm_list_size; i++)
		free(ctl->pcm_list[i]);
	free(ctl->dev_list);
	free(ctl->pcm_list);
	free(ctl->elem_list);
	free(ctl->elem_update_list);
	free(ctl);
}

static int bluealsa_elem_count(snd_ctl_ext_t *ext) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;
	return ctl->elem_list_size;
}

static int bluealsa_elem_list(snd_ctl_ext_t *ext, unsigned int offset, snd_ctl_elem_id_t *id) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;

	if (offset > ctl->elem_list_size)
		return -EINVAL;

	snd_ctl_elem_id_set_numid(id, ctl->elem_list[offset].numid);
	snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_id_set_name(id, ctl->elem_list[offset].name);
	snd_ctl_elem_id_set_index(id, ctl->elem_list[offset].index);

	return 0;
}

static snd_ctl_ext_key_t bluealsa_find_elem(snd_ctl_ext_t *ext, const snd_ctl_elem_id_t *id) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;

	unsigned int numid = snd_ctl_elem_id_get_numid(id);

	if (numid > 0 && numid <= ctl->elem_list_size)
		return numid - 1;

	const char *name = snd_ctl_elem_id_get_name(id);
	unsigned int index = snd_ctl_elem_id_get_index(id);

	for (size_t i = 0; i < ctl->elem_list_size; i++)
		if (strcmp(ctl->elem_list[i].name, name) == 0 &&
				ctl->elem_list[i].index == index)
			return i;

	return SND_CTL_EXT_KEY_NOT_FOUND;
}

static int bluealsa_get_attribute(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key,
		int *type, unsigned int *acc, unsigned int *count) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;

	if (key > ctl->elem_list_size)
		return -EINVAL;

	const struct ctl_elem *elem = &ctl->elem_list[key];
	const struct ba_pcm *pcm = elem->pcm;

	switch (elem->type) {
	case CTL_ELEM_TYPE_BATTERY:
		*acc = SND_CTL_EXT_ACCESS_READ;
		*type = SND_CTL_ELEM_TYPE_INTEGER;
		*count = 1;
		break;
	case CTL_ELEM_TYPE_CODEC:
		*acc = SND_CTL_EXT_ACCESS_READWRITE;
		*type = SND_CTL_ELEM_TYPE_ENUMERATED;
		*count = 1;
		break;
	case CTL_ELEM_TYPE_VOLUME_MODE:
		*acc = SND_CTL_EXT_ACCESS_READWRITE;
		*type = SND_CTL_ELEM_TYPE_ENUMERATED;
		*count = 1;
		break;
	case CTL_ELEM_TYPE_DELAY_SYNC:
		*acc = SND_CTL_EXT_ACCESS_READWRITE;
		*type = SND_CTL_ELEM_TYPE_ENUMERATED;
		*count = 1;
		break;
	case CTL_ELEM_TYPE_SWITCH:
		*acc = SND_CTL_EXT_ACCESS_READWRITE;
		*type = SND_CTL_ELEM_TYPE_BOOLEAN;
		*count = pcm->channels;
		break;
	case CTL_ELEM_TYPE_VOLUME:
		*acc = SND_CTL_EXT_ACCESS_READWRITE |
			SND_CTL_EXT_ACCESS_TLV_CALLBACK |
			SND_CTL_EXT_ACCESS_TLV_READ;
		*type = SND_CTL_ELEM_TYPE_INTEGER;
		*count = pcm->channels;
		break;
	}

	return 0;
}

static int bluealsa_get_integer_info(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key,
		long *imin, long *imax, long *istep) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;

	if (key > ctl->elem_list_size)
		return -EINVAL;

	const struct ctl_elem *elem = &ctl->elem_list[key];

	switch (elem->type) {
	case CTL_ELEM_TYPE_BATTERY:
		*imin = 0;
		*imax = 100;
		*istep = 1;
		break;
	case CTL_ELEM_TYPE_VOLUME:
		switch (elem->pcm->transport) {
		case BA_PCM_TRANSPORT_A2DP_SOURCE:
		case BA_PCM_TRANSPORT_A2DP_SINK:
			*imax = 127;
			break;
		case BA_PCM_TRANSPORT_HFP_AG:
		case BA_PCM_TRANSPORT_HFP_HF:
		case BA_PCM_TRANSPORT_HSP_AG:
		case BA_PCM_TRANSPORT_HSP_HS:
			*imax = 15;
			break;
		default:
			return -EINVAL;
		}
		*imin = 0;
		*istep = 1;
		break;
	case CTL_ELEM_TYPE_CODEC:
	case CTL_ELEM_TYPE_VOLUME_MODE:
	case CTL_ELEM_TYPE_SWITCH:
	case CTL_ELEM_TYPE_DELAY_SYNC:
		return -EINVAL;
	}

	return 0;
}

static snd_mixer_selem_channel_id_t ba_channel_map_to_id(const char *tag) {

	static const struct {
		const char *tag;
		snd_mixer_selem_channel_id_t id;
	} mapping[] = {
		{ "MONO", SND_MIXER_SCHN_MONO },
		{ "FL", SND_MIXER_SCHN_FRONT_LEFT },
		{ "FR", SND_MIXER_SCHN_FRONT_RIGHT },
		{ "RL", SND_MIXER_SCHN_REAR_LEFT },
		{ "RR", SND_MIXER_SCHN_REAR_RIGHT },
		{ "FC", SND_MIXER_SCHN_FRONT_CENTER },
		{ "LFE", SND_MIXER_SCHN_WOOFER },
		{ "SL", SND_MIXER_SCHN_SIDE_LEFT },
		{ "SR", SND_MIXER_SCHN_SIDE_RIGHT },
	};

	for (size_t i = 0; i < ARRAYSIZE(mapping); i++)
		if (strcmp(tag, mapping[i].tag) == 0)
			return mapping[i].id;
	return SND_MIXER_SCHN_UNKNOWN;
}

/**
 * Convert BlueALSA channel index to ALSA mixer simple element channel ID.
 *
 * ALSA mixer does not use channel map to identify channels. Instead, it uses
 * simple element channel ID (index) to identify them. This function converts
 * BlueALSA channel index to ALSA channel index using channel map.
 *
 * @param pcm BlueALSA PCM structure.
 * @param channel BlueALSA PCM channel index.
 * @return The ALSA mixer simple element channel ID. */
static snd_mixer_selem_channel_id_t bluealsa_get_channel_id(const struct ba_pcm *pcm,
		unsigned int channel) {
	snd_mixer_selem_channel_id_t id = ba_channel_map_to_id(pcm->channel_map[channel]);
	/* Make sure that the channel ID is within the valid range. */
	if (id >= 0 && id < pcm->channels)
		return id;
	/* Something went wrong - fallback to the mono channel. */
	SNDERR("Invalid channel map [channel=%u]: %s", channel, pcm->channel_map[channel]);
	return SND_MIXER_SCHN_MONO;
}

static int bluealsa_read_integer(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, long *value) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;

	if (key > ctl->elem_list_size)
		return -EINVAL;

	const struct ctl_elem *elem = &ctl->elem_list[key];
	const struct ba_pcm *pcm = elem->pcm;
	const bool active = elem->active;

	switch (elem->type) {
	case CTL_ELEM_TYPE_BATTERY:
		value[0] = active ? elem->dev->battery_level : 0;
		break;
	case CTL_ELEM_TYPE_SWITCH:
		for (size_t i = 0; i < pcm->channels; i++)
			value[bluealsa_get_channel_id(pcm, i)] = active ? !pcm->volume[i].muted : 0;
		break;
	case CTL_ELEM_TYPE_VOLUME:
		for (size_t i = 0; i < pcm->channels; i++)
			value[bluealsa_get_channel_id(pcm, i)] = active ? pcm->volume[i].volume : 0;
		break;
	case CTL_ELEM_TYPE_CODEC:
	case CTL_ELEM_TYPE_VOLUME_MODE:
	case CTL_ELEM_TYPE_DELAY_SYNC:
		return -EINVAL;
	}

	return 0;
}

static int bluealsa_write_integer(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, long *value) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;

	if (key > ctl->elem_list_size)
		return -EINVAL;

	struct ctl_elem *elem = &ctl->elem_list[key];
	struct ba_pcm *pcm = elem->pcm;

	uint8_t old[ARRAYSIZE(pcm->volume)];
	memcpy(old, pcm->volume, sizeof(old));

	if (!elem->active) {
		/* Ignore the write request because the associated PCM profile has been
		 * disconnected. Create an update event so the application is informed
		 * that the value has been reset to zero. */
		bluealsa_event_elem_updated(ctl, elem);
		bluealsa_pipefd_ping(ctl);
		return 1;
	}

	switch (elem->type) {
	case CTL_ELEM_TYPE_BATTERY:
		/* this element should be read-only */
		return -EINVAL;
	case CTL_ELEM_TYPE_SWITCH:
		for (size_t i = 0; i < pcm->channels; i++)
			pcm->volume[i].muted = !value[bluealsa_get_channel_id(pcm, i)];
		break;
	case CTL_ELEM_TYPE_VOLUME:
		for (size_t i = 0; i < pcm->channels; i++)
			pcm->volume[i].volume = value[bluealsa_get_channel_id(pcm, i)];
		break;
	case CTL_ELEM_TYPE_CODEC:
	case CTL_ELEM_TYPE_VOLUME_MODE:
	case CTL_ELEM_TYPE_DELAY_SYNC:
		return -EINVAL;
	}

	/* check whether update is required */
	if (memcmp(pcm->volume, old, sizeof(old)) == 0)
		return 0;

	if (!ba_dbus_pcm_update(&ctl->dbus_ctx, pcm, BLUEALSA_PCM_VOLUME, NULL))
		return -ENOMEM;

	return 1;
}

int bluealsa_get_enumerated_info(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, unsigned int *items) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;

	if (key > ctl->elem_list_size)
		return -EINVAL;

	const struct ctl_elem *elem = &ctl->elem_list[key];

	switch (elem->type) {
	case CTL_ELEM_TYPE_CODEC:
		*items = elem->codecs.codecs_len;
		break;
	case CTL_ELEM_TYPE_VOLUME_MODE:
		*items = ARRAYSIZE(soft_volume_names);
		break;
	case CTL_ELEM_TYPE_DELAY_SYNC:
		*items = DELAY_SYNC_NUM_VALUES;
		break;
	case CTL_ELEM_TYPE_BATTERY:
	case CTL_ELEM_TYPE_SWITCH:
	case CTL_ELEM_TYPE_VOLUME:
		return -EINVAL;
	}

	return 0;
}

int bluealsa_get_enumerated_name(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key,
		unsigned int item, char *name, size_t name_max_len) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;

	if (key > ctl->elem_list_size)
		return -EINVAL;

	const struct ctl_elem *elem = &ctl->elem_list[key];

	switch (elem->type) {
	case CTL_ELEM_TYPE_CODEC:
		if (item >= elem->codecs.codecs_len)
			return -EINVAL;
		strncpy(name, elem->codecs.codecs[item].name, name_max_len - 1);
		name[name_max_len - 1] = '\0';
		break;
	case CTL_ELEM_TYPE_VOLUME_MODE:
		if (item >= ARRAYSIZE(soft_volume_names))
			return -EINVAL;
		strncpy(name, soft_volume_names[item], name_max_len - 1);
		name[name_max_len - 1] = '\0';
		break;
	case CTL_ELEM_TYPE_DELAY_SYNC:
		if (item >= DELAY_SYNC_NUM_VALUES)
			return -EINVAL;
		const int16_t value = (item * DELAY_SYNC_STEP) + DELAY_SYNC_MIN_VALUE;
		snprintf(name, name_max_len, "%+d ms", value / 10);
		break;
	case CTL_ELEM_TYPE_BATTERY:
	case CTL_ELEM_TYPE_SWITCH:
	case CTL_ELEM_TYPE_VOLUME:
		return -EINVAL;
	}

	return 0;
}

static int bluealsa_read_enumerated(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key,
		unsigned int *items) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;

	if (key > ctl->elem_list_size)
		return -EINVAL;

	const struct ctl_elem *elem = &ctl->elem_list[key];
	const struct ba_pcm *pcm = elem->pcm;
	int ret = 0;

	switch (elem->type) {
	case CTL_ELEM_TYPE_CODEC:
		/* HFP codec is not known until a second or so after the profile
		 * connection is established. In that case we *guess* that mSBC
		 * will be used if available, or CVSD if not, since we do not
		 * want "unknown" as an enumeration item. */
		if (pcm->transport & BA_PCM_TRANSPORT_MASK_HFP &&
				pcm->codec.name[0] == '\0') {
			for (size_t i = 0; i < elem->codecs.codecs_len; i++) {
				if (strcmp("mSBC", elem->codecs.codecs[i].name) == 0) {
					items[0] = i;
					goto finish;
				}
			}
			items[0] = 0;
			break;
		}
		for (size_t i = 0; i < elem->codecs.codecs_len; i++) {
			if (strcmp(pcm->codec.name, elem->codecs.codecs[i].name) == 0) {
				items[0] = i;
				goto finish;
			}
		}
		ret = -EINVAL;
		break;
	case CTL_ELEM_TYPE_VOLUME_MODE:
		items[0] = pcm->soft_volume ? 1 : 0;
		break;
	case CTL_ELEM_TYPE_DELAY_SYNC:
		items[0] = DIV_ROUND(pcm->client_delay - INT16_MIN, DELAY_SYNC_STEP);
		break;
	case CTL_ELEM_TYPE_BATTERY:
	case CTL_ELEM_TYPE_SWITCH:
	case CTL_ELEM_TYPE_VOLUME:
		ret = -EINVAL;
		break;
	}

finish:
	return ret;
}

static void process_events(snd_ctl_ext_t *ext) {

	snd_ctl_elem_id_t *elem_id;
	snd_ctl_elem_id_alloca(&elem_id);

	unsigned int event_mask;
	while (ext->callback->read_event(ext, elem_id, &event_mask) > 0)
		continue;

}

static int bluealsa_write_enumerated(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key,
		unsigned int *items) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;

	if (key > ctl->elem_list_size)
		return -EINVAL;

	const struct ctl_elem *elem = &ctl->elem_list[key];
	struct ba_pcm *pcm = elem->pcm;

	switch (elem->type) {
	case CTL_ELEM_TYPE_CODEC:
		if (items[0] >= elem->codecs.codecs_len)
			return -EINVAL;
		if (strcmp(pcm->codec.name, elem->codecs.codecs[items[0]].name) == 0)
			return 0;
		if (!ba_dbus_pcm_select_codec(&ctl->dbus_ctx, pcm->pcm_path,
					elem->codecs.codecs[items[0]].name, NULL, 0, 0, 0, 0, NULL))
			return -EIO;
		process_events(&ctl->ext);
		break;
	case CTL_ELEM_TYPE_VOLUME_MODE:
		if (items[0] >= ARRAYSIZE(soft_volume_names))
			return -EINVAL;
		const bool soft_volume = items[0] == 1;
		if (pcm->soft_volume == soft_volume)
			return 0;
		pcm->soft_volume = soft_volume;
		if (!ba_dbus_pcm_update(&ctl->dbus_ctx, pcm, BLUEALSA_PCM_SOFT_VOLUME, NULL))
			return -EIO;
		break;
	case CTL_ELEM_TYPE_DELAY_SYNC:
		if (items[0] >= DELAY_SYNC_NUM_VALUES)
			return -EINVAL;
		const int16_t delay = items[0] * DELAY_SYNC_STEP + DELAY_SYNC_MIN_VALUE;
		if (pcm->client_delay == delay)
			return 0;
		pcm->client_delay = delay;
		if (!ba_dbus_pcm_update(&ctl->dbus_ctx, pcm, BLUEALSA_PCM_CLIENT_DELAY, NULL))
			return -EIO;
		process_events(&ctl->ext);
		break;
	case CTL_ELEM_TYPE_BATTERY:
	case CTL_ELEM_TYPE_SWITCH:
	case CTL_ELEM_TYPE_VOLUME:
		return -EINVAL;
	}

	return 1;
}

static void bluealsa_subscribe_events(snd_ctl_ext_t *ext, int subscribe) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;

	if (subscribe) {
		ba_dbus_connection_signal_match_add(&ctl->dbus_ctx, ctl->dbus_ctx.ba_service, NULL,
				DBUS_INTERFACE_OBJECT_MANAGER, "InterfacesAdded", "path_namespace='/org/bluealsa'");
		ba_dbus_connection_signal_match_add(&ctl->dbus_ctx, ctl->dbus_ctx.ba_service, NULL,
				DBUS_INTERFACE_OBJECT_MANAGER, "InterfacesRemoved", "path_namespace='/org/bluealsa'");
		char dbus_args[50];
		snprintf(dbus_args, sizeof(dbus_args), "arg0='%s',arg2=''", ctl->dbus_ctx.ba_service);
		ba_dbus_connection_signal_match_add(&ctl->dbus_ctx, DBUS_SERVICE_DBUS, NULL,
				DBUS_INTERFACE_DBUS, "NameOwnerChanged", dbus_args);
		ba_dbus_connection_signal_match_add(&ctl->dbus_ctx, ctl->dbus_ctx.ba_service, NULL,
				DBUS_INTERFACE_PROPERTIES, "PropertiesChanged", "arg0='"BLUEALSA_INTERFACE_PCM"'");
		ba_dbus_connection_signal_match_add(&ctl->dbus_ctx, ctl->dbus_ctx.ba_service, NULL,
				DBUS_INTERFACE_PROPERTIES, "PropertiesChanged", "arg0='"BLUEALSA_INTERFACE_RFCOMM"'");
		ba_dbus_connection_signal_match_add(&ctl->dbus_ctx, "org.bluez", NULL,
				DBUS_INTERFACE_PROPERTIES, "PropertiesChanged", "arg0='org.bluez.Device1'");
	}
	else
		ba_dbus_connection_signal_match_clean(&ctl->dbus_ctx);

	dbus_connection_flush(ctl->dbus_ctx.conn);
}

static dbus_bool_t bluealsa_dbus_msg_update_dev(const char *key,
		DBusMessageIter *value, void *userdata, DBusError *error) {
	(void)error;

	struct bt_dev *dev = (struct bt_dev *)userdata;
	dev->mask = BT_DEV_MASK_NONE;

	if (dbus_message_iter_get_arg_type(value) != DBUS_TYPE_VARIANT)
		return FALSE;

	DBusMessageIter variant;
	dbus_message_iter_recurse(value, &variant);

	if (strcmp(key, "Alias") == 0) {
		const char *alias;
		dbus_message_iter_get_basic(&variant, &alias);
		*stpncpy(dev->name, alias, sizeof(dev->name) - 1) = '\0';
		dev->mask = BT_DEV_MASK_UPDATE;
	}
	else if (strcmp(key, "Battery") == 0) {
		signed char level;
		dbus_message_iter_get_basic(&variant, &level);
		dev->mask = BT_DEV_MASK_UPDATE;
		if (dev->battery_level == -1)
			dev->mask = BT_DEV_MASK_ADD | BT_DEV_MASK_UPDATE;
		dev->battery_level = level;
	}
	else if (strcmp(key, "Connected") == 0) {
		dbus_bool_t connected;
		dbus_message_iter_get_basic(&variant, &connected);
		/* process device disconnected event only */
		if (!connected)
			dev->mask = BT_DEV_MASK_REMOVE;
	}

	return TRUE;
}

static DBusHandlerResult bluealsa_dbus_msg_filter(DBusConnection *conn,
		DBusMessage *message, void *data) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)data;
	(void)conn;

	if (dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_SIGNAL)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	DBusMessageIter iter;
	if (!dbus_message_iter_init(message, &iter))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	const char *path = dbus_message_get_path(message);
	const char *interface = dbus_message_get_interface(message);
	const char *signal = dbus_message_get_member(message);

	if (strcmp(interface, DBUS_INTERFACE_PROPERTIES) == 0 &&
			strcmp(signal, "PropertiesChanged") == 0) {

		const char *updated_interface;
		dbus_message_iter_get_basic(&iter, &updated_interface);
		dbus_message_iter_next(&iter);

		/* handle BlueZ device properties update */
		if (strcmp(updated_interface, "org.bluez.Device1") == 0)
			for (size_t i = 0; i < ctl->elem_list_size; i++) {
				struct bt_dev *dev = ctl->elem_list[i].dev;
				if (strcmp(dev->device_path, path) == 0) {
					dbus_message_iter_dict(&iter, NULL,
							bluealsa_dbus_msg_update_dev, dev);
					if (dev->mask & BT_DEV_MASK_UPDATE)
						goto remove_add;
					if (ctl->single_device &&
							dev->mask & BT_DEV_MASK_REMOVE) {
						/* Single device mode does not process PCM removes, however,
						 * when the device disconnects we would like to simulate CTL
						 * unplug event. */
						ctl->pcm_list_size = 0;
						goto remove_add;
					}
				}
			}

		/* handle BlueALSA RFCOMM properties update */
		if (strcmp(updated_interface, BLUEALSA_INTERFACE_RFCOMM) == 0)
			for (size_t i = 0; i < ctl->elem_list_size; i++) {
				struct ctl_elem *elem = &ctl->elem_list[i];
				struct bt_dev *dev = elem->dev;
				if (strcmp(dev->rfcomm_path, path) == 0) {
					dbus_message_iter_dict(&iter, NULL,
							bluealsa_dbus_msg_update_dev, dev);
					/* for non-dynamic mode we need to use update logic */
					if (ctl->dynamic &&
							dev->mask & BT_DEV_MASK_ADD)
						goto remove_add;
					if (elem->type != CTL_ELEM_TYPE_BATTERY)
						continue;
					if (dev->mask & BT_DEV_MASK_UPDATE)
						bluealsa_event_elem_updated(ctl, elem);
				}
			}

		/* handle BlueALSA PCM properties update */
		if (strcmp(updated_interface, BLUEALSA_INTERFACE_PCM) == 0)
			for (size_t i = 0; i < ctl->elem_list_size; i++) {
				struct ctl_elem *elem = &ctl->elem_list[i];
				struct ba_pcm *pcm = elem->pcm;
				if (elem->type == CTL_ELEM_TYPE_BATTERY)
					continue;
				if (strcmp(pcm->pcm_path, path) == 0) {
					dbus_message_iter_get_ba_pcm_props(&iter, NULL, pcm);
					bluealsa_event_elem_updated(ctl, elem);
				}
			}

	}
	else if (strcmp(interface, DBUS_INTERFACE_OBJECT_MANAGER) == 0) {

		if (strcmp(signal, "InterfacesAdded") == 0) {
			struct ba_pcm pcm;
			if (dbus_message_iter_get_ba_pcm(&iter, NULL, &pcm) &&
					pcm.transport != BA_PCM_TRANSPORT_NONE) {

				if (ctl->dynamic)
					bluealsa_pcm_add(ctl, &pcm);
				else
					bluealsa_pcm_activate(ctl, &pcm);

				goto remove_add;

			}
		}

		if (strcmp(signal, "InterfacesRemoved") == 0) {

			const char *pcm_path;
			dbus_message_iter_get_basic(&iter, &pcm_path);

			if (ctl->dynamic)
				bluealsa_pcm_remove(ctl, pcm_path);
			else
				/* In the non-dynamic operation mode we never remove any elements,
				 * we simply mark all elements of the removed PCM as inactive. */
				bluealsa_pcm_deactivate(ctl, pcm_path);

			goto remove_add;

		}

	}
	else if (strcmp(interface, DBUS_INTERFACE_DBUS) == 0 &&
			strcmp(signal, "NameOwnerChanged") == 0) {

		const char *service, *arg2;
		dbus_message_iter_get_basic(&iter, &service);
		if (strcmp(service, ctl->dbus_ctx.ba_service) == 0) {
			if (dbus_message_iter_next(&iter) &&
					dbus_message_iter_next(&iter) &&
					dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING) {
				dbus_message_iter_get_basic(&iter, &arg2);
				if (strlen(arg2) == 0) {
					/* BlueALSA daemon has terminated,
					 * so all PCMs have been removed. */
					ctl->pcm_list_size = 0;
					goto remove_add;
				}
			}
		}

	}

	return DBUS_HANDLER_RESULT_HANDLED;

remove_add:

	if (!ctl->dynamic)
		/* non-dynamic mode SHALL not add/remove any elements */
		goto final;

	/* During a PCM name change, new PCM insertion and/or deletion, the name
	 * of all control elements might have change, because of optional unique
	 * device ID suffix - for more information see the bluealsa_elem_set_name()
	 * function. So, in such a case we will simply remove all old controllers
	 * and add new ones in order to update potential name changes. */

	for (size_t i = 0; i < ctl->elem_list_size; i++)
		bluealsa_event_elem_removed(ctl, &ctl->elem_list[i]);

	bluealsa_free_elem_list(ctl);
	bluealsa_create_elem_list(ctl);

	for (size_t i = 0; i < ctl->elem_list_size; i++)
		bluealsa_event_elem_added(ctl, &ctl->elem_list[i]);

final:

	if (ctl->single_device && ctl->pcm_list_size == 0) {
		/* Trigger POLLERR by closing the read end of our pipe. This
		 * simulates a CTL device being unplugged. */
		close(ctl->pipefd[0]);
		ctl->pipefd[0] = -1;
	}

	return DBUS_HANDLER_RESULT_HANDLED;
}

static int bluealsa_read_event(snd_ctl_ext_t *ext, snd_ctl_elem_id_t *id, unsigned int *event_mask) {
	struct bluealsa_ctl *ctl = ext->private_data;

	/* Some applications (e.g. MPD) ignore POLLERR and rely on snd_ctl_read()
	 * to return an appropriate error code. So we check the state of our
	 * device disconnection pipe and return -ENODEV if the device is
	 * disconnected. */
	if (ctl->single_device && ctl->pipefd[0] == -1)
		return -ENODEV;

	if (ctl->elem_update_list_size) {

		const struct ctl_elem_update *update = &ctl->elem_update_list[ctl->elem_update_event_i];

		snd_ctl_elem_id_set_numid(id, update->numid);
		snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
		snd_ctl_elem_id_set_name(id, update->name);
		snd_ctl_elem_id_set_index(id, update->index);
		*event_mask = update->event_mask;

		if (++ctl->elem_update_event_i == ctl->elem_update_list_size) {
			ctl->elem_update_list_size = 0;
			ctl->elem_update_event_i = 0;
		}

		return 1;
	}

	/* The ALSA snd_mixer API does not propagate the
	 * snd_mixer_poll_descriptors_revents() call down to the underlying hctl
	 * API, so our .poll_revents callback is never invoked by applications
	 * using the snd_mixer API (i.e. just about every mixer application!).
	 * But we need to feed poll() events back to our dispatching function.
	 * Since ALSA is not cooperating, we will call poll() once more by
	 * ourself and receive required event flags. If someday ALSA will be so
	 * kind to actually call .poll_revents(), this code should remain as a
	 * backward compatibility. */
	ba_dbus_connection_dispatch(&ctl->dbus_ctx);
	/* For the same reason, we also need to clear any internal ping events. */
	if (ctl->single_device)
		bluealsa_pipefd_flush(ctl);

	if (ctl->elem_update_list_size)
		return bluealsa_read_event(ext, id, event_mask);
	return -EAGAIN;
}

static int bluealsa_poll_descriptors_count(snd_ctl_ext_t *ext) {
	struct bluealsa_ctl *ctl = ext->private_data;

	nfds_t nfds = 0;
	ba_dbus_connection_poll_fds(&ctl->dbus_ctx, NULL, &nfds);

	if (ctl->pipefd[0] > -1)
		++nfds;
	if (ctl->pipefd[1] > -1)
		++nfds;

	return nfds;
}

static int bluealsa_poll_descriptors(snd_ctl_ext_t *ext, struct pollfd *pfd,
		unsigned int nfds) {
	struct bluealsa_ctl *ctl = ext->private_data;

	nfds_t pipe_nfds = 0;

	/* Just in case some application (MPD ???) cannot handle a pfd with
	 * .fd == -1, we omit each end of the pipe from the poll() if it is
	 * already closed. */

	if (ctl->pipefd[0] > -1) {
		pfd[pipe_nfds].fd = ctl->pipefd[0];
		pfd[pipe_nfds].events = POLLIN;
		pipe_nfds++;
	}

	if (ctl->pipefd[1] > -1) {
		pfd[pipe_nfds].fd = ctl->pipefd[1];
		/* For the write end of our internal PIPE we are not interested
		 * in any I/O events, only in error condition. */
		pfd[pipe_nfds].events = 0;
		pipe_nfds++;
	}

	nfds_t dbus_nfds = nfds - pipe_nfds;
	if (!ba_dbus_connection_poll_fds(&ctl->dbus_ctx, &pfd[pipe_nfds], &dbus_nfds))
		return -EINVAL;

	return pipe_nfds + dbus_nfds;
}

static int bluealsa_poll_revents(snd_ctl_ext_t *ext, struct pollfd *pfd,
		unsigned int nfds, unsigned short *revents) {
	struct bluealsa_ctl *ctl = ext->private_data;

	nfds_t pipe_nfds = 0;
	*revents = 0;

	if (ctl->pipefd[0] > -1) {
		if (pfd[0].revents)
			bluealsa_pipefd_flush(ctl);
		*revents |= pfd[pipe_nfds].revents;
		pipe_nfds++;
	}

	if (ctl->pipefd[1] > -1) {
		*revents |= pfd[pipe_nfds].revents;
		pipe_nfds++;
	}

	if (ba_dbus_connection_poll_dispatch(&ctl->dbus_ctx, &pfd[pipe_nfds], nfds - pipe_nfds))
		*revents |= POLLIN;

	return 0;
}

static const snd_ctl_ext_callback_t bluealsa_snd_ctl_ext_callback = {
	.close = bluealsa_close,
	.elem_count = bluealsa_elem_count,
	.elem_list = bluealsa_elem_list,
	.find_elem = bluealsa_find_elem,
	.get_attribute = bluealsa_get_attribute,
	.get_integer_info = bluealsa_get_integer_info,
	.get_enumerated_info = bluealsa_get_enumerated_info,
	.get_enumerated_name = bluealsa_get_enumerated_name,
	.read_integer = bluealsa_read_integer,
	.read_enumerated = bluealsa_read_enumerated,
	.write_integer = bluealsa_write_integer,
	.write_enumerated = bluealsa_write_enumerated,
	.subscribe_events = bluealsa_subscribe_events,
	.read_event = bluealsa_read_event,
	.poll_descriptors_count = bluealsa_poll_descriptors_count,
	.poll_descriptors = bluealsa_poll_descriptors,
	.poll_revents = bluealsa_poll_revents,
};

#if SND_CTL_EXT_VERSION >= 0x010001
#define TLV_DB_RANGE_SCALE_MIN_MAX(min, max, min_dB, max_dB) \
	(min), (max), 4 /* dB min/max scale */, 2 * sizeof(int), (min_dB), (max_dB)
static int bluealsa_snd_ctl_ext_tlv_callback(snd_ctl_ext_t *ext,
		snd_ctl_ext_key_t key, int op_flag, unsigned int numid,
		unsigned int *tlv, unsigned int tlv_size) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;
	(void)numid;

	static const unsigned int tlv_db_a2dp[] = {
		3,  /* dB range container */
		10 * (2 /* range */ + 4 /* dB scale */) * sizeof(int),
		TLV_DB_RANGE_SCALE_MIN_MAX(0, 1, -9600, -6988),
		TLV_DB_RANGE_SCALE_MIN_MAX(2, 3, -5988, -5403),
		TLV_DB_RANGE_SCALE_MIN_MAX(4, 5, -4988, -4666),
		TLV_DB_RANGE_SCALE_MIN_MAX(6, 8, -4399, -3984),
		TLV_DB_RANGE_SCALE_MIN_MAX(9, 13, -3806, -3277),
		TLV_DB_RANGE_SCALE_MIN_MAX(14, 21, -3163, -2580),
		TLV_DB_RANGE_SCALE_MIN_MAX(22, 35, -2504, -1837),
		TLV_DB_RANGE_SCALE_MIN_MAX(36, 59, -1788, -1081),
		TLV_DB_RANGE_SCALE_MIN_MAX(60, 100, -1048, -317),
		TLV_DB_RANGE_SCALE_MIN_MAX(101, 127, -324, 0),
	};

	static const unsigned int tlv_db_sco[] = {
		3,  /* dB range container */
		6 * (2 /* range */ + 4 /* dB scale */) * sizeof(int),
		TLV_DB_RANGE_SCALE_MIN_MAX(0, 1, -9600, -3906),
		TLV_DB_RANGE_SCALE_MIN_MAX(2, 3, -2906, -2321),
		TLV_DB_RANGE_SCALE_MIN_MAX(4, 5, -1906, -1584),
		TLV_DB_RANGE_SCALE_MIN_MAX(6, 7, -1321, -1099),
		TLV_DB_RANGE_SCALE_MIN_MAX(8, 10, -904, -582),
		TLV_DB_RANGE_SCALE_MIN_MAX(11, 15, -438, 0),
	};

	const struct ctl_elem *elem = &ctl->elem_list[key];
	const unsigned int *tlv_db = NULL;
	size_t tlv_db_size = 0;

	switch (elem->pcm->transport) {
	case BA_PCM_TRANSPORT_A2DP_SOURCE:
	case BA_PCM_TRANSPORT_A2DP_SINK:
		tlv_db_size = sizeof(tlv_db_a2dp);
		tlv_db = tlv_db_a2dp;
		break;
	case BA_PCM_TRANSPORT_HFP_AG:
	case BA_PCM_TRANSPORT_HFP_HF:
	case BA_PCM_TRANSPORT_HSP_AG:
	case BA_PCM_TRANSPORT_HSP_HS:
		tlv_db_size = sizeof(tlv_db_sco);
		tlv_db = tlv_db_sco;
		break;
	default:
		return -ENXIO;
	}

	if (op_flag != 0)
		return -ENXIO;
	if (tlv_size < tlv_db_size)
		return -ENOMEM;

	memcpy(tlv, tlv_db, tlv_db_size);
	return 0;
}
#endif

SND_CTL_PLUGIN_DEFINE_FUNC(bluealsa) {
	(void)root;

	DBusError err = DBUS_ERROR_INIT;
	const char *service = BLUEALSA_SERVICE;
	const char *device = NULL;
	const char *extended = NULL;
	bool show_battery = false;
	bool show_bt_transport = false;
	bool show_codec = false;
	bool show_vol_mode = false;
	bool show_delay_sync = false;
	bool dynamic = true;
	struct bluealsa_ctl *ctl;
	int ret;

	snd_config_iterator_t pos, next;
	snd_config_for_each(pos, next, conf) {
		snd_config_t *n = snd_config_iterator_entry(pos);

		const char *id;
		if (snd_config_get_id(n, &id) < 0)
			continue;

		if (strcmp(id, "comment") == 0 ||
				strcmp(id, "type") == 0 ||
				strcmp(id, "hint") == 0)
			continue;

		if (strcmp(id, "service") == 0) {
			if (snd_config_get_string(n, &service) < 0) {
				SNDERR("Invalid type for %s", id);
				return -EINVAL;
			}
			continue;
		}
		if (strcmp(id, "device") == 0) {
			if (snd_config_get_string(n, &device) < 0) {
				SNDERR("Invalid type for %s", id);
				return -EINVAL;
			}
			continue;
		}
		if (strcmp(id, "extended") == 0) {
			if ((ret = snd_config_get_string(n, &extended)) < 0) {
				SNDERR("Invalid type for %s", id);
				return -EINVAL;
			}
			if (parse_extended(extended, &show_codec, &show_vol_mode,
						&show_delay_sync, &show_battery) < 0) {
				SNDERR("Invalid extended options: %s", extended);
				return -EINVAL;
			}
			continue;
		}
		if (strcmp(id, "bttransport") == 0) {
			if ((ret = snd_config_get_bool(n)) < 0) {
				SNDERR("Invalid type for %s", id);
				return -EINVAL;
			}
			show_bt_transport = !!ret;
			continue;
		}
		if (strcmp(id, "dynamic") == 0) {
			if ((ret = snd_config_get_bool(n)) < 0) {
				SNDERR("Invalid type for %s", id);
				return -EINVAL;
			}
			dynamic = !!ret;
			continue;
		}

		SNDERR("Unknown field %s", id);
		return -EINVAL;
	}

	bdaddr_t ba_addr = *BDADDR_ALL;
	if (device != NULL && str2bdaddr(device, &ba_addr) == -1) {
		SNDERR("Invalid BT device address: %s", device);
		return -EINVAL;
	}

	/* single Bluetooth device mode */
	bool single_device_mode = bacmp(&ba_addr, BDADDR_ALL) != 0;

	/* non-dynamic operation requires single device mode */
	if (!single_device_mode)
		dynamic = true;

	if ((ctl = calloc(1, sizeof(*ctl))) == NULL)
		return -ENOMEM;

	ctl->ext.version = SND_CTL_EXT_VERSION;
	ctl->ext.card_idx = 0;

	strncpy(ctl->ext.id, "bluealsa", sizeof(ctl->ext.id) - 1);
	strncpy(ctl->ext.driver, "BlueALSA", sizeof(ctl->ext.driver) - 1);
	strncpy(ctl->ext.name, "BlueALSA", sizeof(ctl->ext.name) - 1);
	strncpy(ctl->ext.longname, "Bluetooth Audio Hub Controller", sizeof(ctl->ext.longname) - 1);
	strncpy(ctl->ext.mixername, "BlueALSA Plugin", sizeof(ctl->ext.mixername) - 1);

	ctl->ext.callback = &bluealsa_snd_ctl_ext_callback;
#if SND_CTL_EXT_VERSION >= 0x010001
	ctl->ext.tlv.c = bluealsa_snd_ctl_ext_tlv_callback;
#endif
	ctl->ext.private_data = ctl;
	ctl->ext.poll_fd = -1;

	ctl->pipefd[0] = -1;
	ctl->pipefd[1] = -1;

	ctl->show_codec = show_codec;
	ctl->show_vol_mode = show_vol_mode;
	ctl->show_delay_sync = show_delay_sync;
	ctl->show_battery = show_battery;
	ctl->show_bt_transport = show_bt_transport;
	ctl->single_device = single_device_mode;
	ctl->dynamic = dynamic;

	struct ba_pcm *pcm_list = NULL;
	size_t pcm_list_size = 0;

	dbus_threads_init_default();

	if (!ba_dbus_connection_ctx_init(&ctl->dbus_ctx, service, &err)) {
		SNDERR("Couldn't initialize D-Bus context: %s", err.message);
		ret = -dbus_error_to_errno(&err);
		goto fail;
	}

	if (!dbus_connection_add_filter(ctl->dbus_ctx.conn, bluealsa_dbus_msg_filter, ctl, NULL)) {
		SNDERR("Couldn't add D-Bus filter: %s", strerror(ENOMEM));
		ret = -ENOMEM;
		goto fail;
	}

	if (!ba_dbus_pcm_get_all(&ctl->dbus_ctx, &pcm_list, &pcm_list_size, &err)) {
		SNDERR("Couldn't get BlueALSA PCM list: %s", err.message);
		ret = -dbus_error_to_errno(&err);
		goto fail;
	}

	if (ctl->single_device) {

		if (bacmp(&ba_addr, BDADDR_ANY) == 0) {
			/* Interpret BT address ANY as a request for the most
			 * recently connected Bluetooth audio device. */

			const struct ba_pcm *latest = NULL;
			uint32_t seq = 0;

			if (pcm_list_size == 0) {
				SNDERR("No BlueALSA audio devices connected");
				ret = -ENODEV;
				goto fail;
			}

			for (size_t i = 0; i < pcm_list_size; i++) {
				if (pcm_list[i].sequence >= seq) {
					seq = pcm_list[i].sequence;
					latest = &pcm_list[i];
				}
			}

			bacpy(&ba_addr, &latest->addr);

		}

		/* Filter the PCM list so that it contains only those
		 * PCMs belonging to the selected BT device. */
		size_t count = 0;
		for (size_t i = 0; i < pcm_list_size; i++)
			if (bacmp(&ba_addr, &pcm_list[i].addr) == 0)
				memmove(&pcm_list[count++], &pcm_list[i], sizeof(*pcm_list));
		pcm_list_size = count;

	}

	/* add PCMs to CTL internal PCM list */
	for (size_t i = 0; i < pcm_list_size; i++)
		if (bluealsa_pcm_add(ctl, &pcm_list[i]) == -1) {
			SNDERR("Couldn't add BlueALSA PCM: %s", strerror(errno));
			ret = -errno;
			goto fail;
		}

	free(pcm_list);
	pcm_list = NULL;

	if (bluealsa_create_elem_list(ctl) == -1) {
		SNDERR("Couldn't create control elements: %s", strerror(errno));
		ret = -errno;
		goto fail;
	}

	if (ctl->single_device) {

		if (ctl->dev_list_size != 1) {
			SNDERR("No such BlueALSA audio device: %s", device);
			ret = -ENODEV;
			goto fail;
		}

		if (pipe2(ctl->pipefd, O_CLOEXEC | O_NONBLOCK) == -1) {
			SNDERR("Couldn't create event pipe: %s", strerror(errno));
			ret = -errno;
			goto fail;
		}

		/* use Bluetooth device name as the card name for our plug-in */
		strncpy(ctl->ext.name, ctl->dev_list[0]->name, sizeof(ctl->ext.name) - 1);
		ctl->ext.name[sizeof(ctl->ext.name) - 1] = '\0';

	}

	if ((ret = snd_ctl_ext_create(&ctl->ext, name, mode)) < 0)
		goto fail;

	*handlep = ctl->ext.handle;
	return 0;

fail:
	bluealsa_close(&ctl->ext);
	dbus_error_free(&err);
	free(pcm_list);
	return ret;
}

SND_CTL_PLUGIN_SYMBOL(bluealsa)
