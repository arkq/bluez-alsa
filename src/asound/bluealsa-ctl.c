/*
 * bluealsa-ctl.c
 * Copyright (c) 2016-2022 Arkadiusz Bokowy
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
#include <sys/param.h>
#include <unistd.h>

#include <alsa/asoundlib.h>
#include <alsa/control_external.h>
#include <bluetooth/bluetooth.h>
#include <dbus/dbus.h>

#include "shared/dbus-client.h"
#include "shared/defs.h"

enum ctl_elem_type {
	CTL_ELEM_TYPE_BATTERY,
	CTL_ELEM_TYPE_SWITCH,
	CTL_ELEM_TYPE_VOLUME,
};

struct ctl_elem {
	enum ctl_elem_type type;
	struct bt_dev *dev;
	struct ba_pcm *pcm;
	char name[44 /* internal ALSA constraint */ + 1];
	/* if true, element is a playback control */
	bool playback;
	/* For single device mode, if true then the associated profile is connected.
	 * If false, the element value is zero, and writes are ignored. */
	bool active;
};

struct ctl_elem_update {
	char name[sizeof(((struct ctl_elem *)0)->name)];
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
	struct ba_pcm *pcm_list;
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

	/* if true, show battery meter */
	bool show_battery;
	/* if true, this mixer is for a single Bluetooth device */
	bool single_device;
	/* if true, this mixer adds/removes controls dynamically */
	bool dynamic;

};

static int str2bdaddr(const char *str, bdaddr_t *ba) {

	unsigned int x[6];
	if (sscanf(str, "%x:%x:%x:%x:%x:%x",
				&x[5], &x[4], &x[3], &x[2], &x[1], &x[0]) != 6)
		return -1;

	size_t i;
	for (i = 0; i < 6; i++)
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

	if ((rv = strcmp(e1->name, e2->name)) == 0)
		rv = bacmp(&e1->pcm->addr, &e2->pcm->addr);

	return rv;
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

	size_t i;
	for (i = 0; i < ctl->dev_list_size; i++)
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

/**
 * Get BT device structure.
 *
 * @param ctl The BlueALSA controller context.
 * @param pcm BlueALSA PCM structure.
 * @return The BT device, or NULL upon error. */
static struct bt_dev *bluealsa_dev_get(struct bluealsa_ctl *ctl, const struct ba_pcm *pcm) {

	size_t i;
	for (i = 0; i < ctl->dev_list_size; i++)
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

static int bluealsa_elem_update_list_add(struct bluealsa_ctl *ctl,
		const char *elem_name, unsigned int mask) {

	struct ctl_elem_update *tmp = ctl->elem_update_list;
	if ((tmp = realloc(tmp, (ctl->elem_update_list_size + 1) * sizeof(*tmp))) == NULL)
		return -1;

	tmp[ctl->elem_update_list_size].event_mask = mask;
	*stpncpy(tmp[ctl->elem_update_list_size].name, elem_name,
			sizeof(tmp[ctl->elem_update_list_size].name) - 1) = '\0';

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

static int bluealsa_pcm_add(struct bluealsa_ctl *ctl, const struct ba_pcm *pcm) {
	struct ba_pcm *tmp = ctl->pcm_list;
	if ((tmp = realloc(tmp, (ctl->pcm_list_size + 1) * sizeof(*tmp))) == NULL)
		return -1;
	memcpy(&tmp[ctl->pcm_list_size++], pcm, sizeof(*tmp));
	ctl->pcm_list = tmp;
	return 0;
}

static int bluealsa_pcm_remove(struct bluealsa_ctl *ctl, const char *path) {
	size_t i;
	for (i = 0; i < ctl->pcm_list_size; i++)
		if (strcmp(ctl->pcm_list[i].pcm_path, path) == 0)
			memcpy(&ctl->pcm_list[i], &ctl->pcm_list[--ctl->pcm_list_size], sizeof(*ctl->pcm_list));
	return 0;
}

static int bluealsa_pcm_activate(struct bluealsa_ctl *ctl, const struct ba_pcm *pcm) {
	size_t i;
	for (i = 0; i < ctl->pcm_list_size; i++)
		if (strcmp(ctl->pcm_list[i].pcm_path, pcm->pcm_path) == 0) {

			/* update potentially stalled PCM data */
			memcpy(&ctl->pcm_list[i], pcm, sizeof(ctl->pcm_list[i]));

			size_t el;
			/* activate associated elements */
			for (el = 0; el < ctl->elem_list_size; el++)
				if (ctl->elem_list[el].pcm == &ctl->pcm_list[i]) {
					ctl->elem_list[el].active = true;
					bluealsa_event_elem_updated(ctl, ctl->elem_list[el].name);
				}

			break;
		}
	return 0;
}

static int bluealsa_pcm_deactivate(struct bluealsa_ctl *ctl, const char *path) {
	size_t i;
	for (i = 0; i < ctl->elem_list_size; i++)
		if (strcmp(ctl->elem_list[i].pcm->pcm_path, path) == 0) {
			ctl->elem_list[i].active = false;
			bluealsa_event_elem_updated(ctl, ctl->elem_list[i].name);
		}
	return 0;
}

/**
 * Update element name based on given string and PCM type.
 *
 * @param elem An address to the element structure.
 * @param name A string which should be used as a base for the element name. May
 *   be NULL if no base prefix is required.
 * @param id An unique ID number. If the ID is other than -1, it will be
 *   attached to the element name in order to prevent duplications. */
static void bluealsa_elem_set_name(struct ctl_elem *elem, const char *name, int id) {

	if (name != NULL) {
		/* multi-device mixer - include device alias in control names */

		const int name_len = strlen(name);
		int len = sizeof(elem->name) - 16 - 1;
		char no[16] = "";

		if (id != -1) {
			sprintf(no, " #%u", id);
			len -= strlen(no);
		}

		if (elem->type == CTL_ELEM_TYPE_BATTERY) {
			len = MIN(len - 10, name_len);
			while (isspace(name[len - 1]))
				len--;
			sprintf(elem->name, "%.*s%s | Battery", len, name, no);
		}
		else {
			/* avoid name duplication by adding profile suffixes */
			switch (elem->pcm->transport) {
			case BA_PCM_TRANSPORT_A2DP_SOURCE:
			case BA_PCM_TRANSPORT_A2DP_SINK:
				len = MIN(len - 7, name_len);
				while (isspace(name[len - 1]))
					len--;
				sprintf(elem->name, "%.*s%s - A2DP", len, name, no);
				break;
			case BA_PCM_TRANSPORT_HFP_AG:
			case BA_PCM_TRANSPORT_HFP_HF:
			case BA_PCM_TRANSPORT_HSP_AG:
			case BA_PCM_TRANSPORT_HSP_HS:
				len = MIN(len - 6, name_len);
				while (isspace(name[len - 1]))
					len--;
				sprintf(elem->name, "%.*s%s - SCO", len, name, no);
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
				strcpy(elem->name, "A2DP");
				break;
			case BA_PCM_TRANSPORT_HFP_AG:
			case BA_PCM_TRANSPORT_HFP_HF:
			case BA_PCM_TRANSPORT_HSP_AG:
			case BA_PCM_TRANSPORT_HSP_HS:
				strcpy(elem->name, "SCO");
				break;
			}
	}

	/* ALSA library determines the element type by checking it's
	 * name suffix. This feature is not well documented, though. */

	strcat(elem->name, elem->playback ? " Playback" : " Capture");

	switch (elem->type) {
	case CTL_ELEM_TYPE_SWITCH:
		strcat(elem->name, " Switch");
		break;
	case CTL_ELEM_TYPE_BATTERY:
	case CTL_ELEM_TYPE_VOLUME:
		strcat(elem->name, " Volume");
		break;
	}

}

/**
 * Create control elements for a given PCM.
 *
 * @param elem_list An address to the array of element structures. This array
 *   must have sufficient space for new elements which includes volume element,
 *   switch element and optional battery indicator element.
 * @param dev The BT device associated with created elements.
 * @param pcm The BlueALSA PCM associated with created elements.
 * @param single_device If true, elements shall be created for the single
 *   device mode.
 * @param add_battery_elem If true, try to add an optional battery level
 *   indicator element.
 * @return The number of elements added. */
static size_t elem_list_add_pcm_elems(struct ctl_elem *elem_list, struct bt_dev *dev,
		struct ba_pcm *pcm, bool single_device, bool add_battery_elem) {

	const char *name = single_device ? NULL : dev->name;
	size_t n = 0;

	elem_list[n].type = CTL_ELEM_TYPE_VOLUME;
	elem_list[n].dev = dev;
	elem_list[n].pcm = pcm;
	elem_list[n].playback = pcm->mode == BA_PCM_MODE_SINK;
	elem_list[n].active = true;
	bluealsa_elem_set_name(&elem_list[n], name, -1);

	n++;

	elem_list[n].type = CTL_ELEM_TYPE_SWITCH;
	elem_list[n].dev = dev;
	elem_list[n].pcm = pcm;
	elem_list[n].playback = pcm->mode == BA_PCM_MODE_SINK;
	elem_list[n].active = true;
	bluealsa_elem_set_name(&elem_list[n], name, -1);

	n++;

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
		bluealsa_elem_set_name(&elem_list[n], name, -1);

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
	size_t i;
	struct ctl_elem *elem_list = ctl->elem_list;

	if (ctl->pcm_list_size > 0) {

		for (i = 0; i < ctl->pcm_list_size; i++) {
			/* Every stream has two controls associated to
			 * itself - volume adjustment and mute switch. */
			count += 2;
			/* It is possible, that BT device battery level will be exposed via
			 * RFCOMM interface, so in order to account for a special "battery"
			 * element we have to increment our element counter by one. */
			count += 1;
		}

		if ((elem_list = realloc(elem_list, count * sizeof(*elem_list))) == NULL)
			return -1;

	}

	/* Clear device mask, so we can distinguish currently used and unused (old)
	 * device entries - we are not invalidating device list after PCM remove. */
	for (i = 0; i < ctl->dev_list_size; i++)
		ctl->dev_list[i]->mask = BT_DEV_MASK_NONE;

	count = 0;

	/* Construct control elements based on available PCMs. */
	for (i = 0; i < ctl->pcm_list_size; i++) {

		struct ba_pcm *pcm = &ctl->pcm_list[i];
		struct bt_dev *dev = bluealsa_dev_get(ctl, pcm);
		bool add_battery_elem = false;

		if (ctl->show_battery &&
				!elem_list_dev_has_battery_elem(elem_list, count, dev)) {
			bluealsa_dev_fetch_battery(ctl, dev);
			add_battery_elem = true;
		}

		count += elem_list_add_pcm_elems(&elem_list[count],
				dev, pcm, ctl->single_device, add_battery_elem);

	}

	/* Sort control elements alphabetically. */
	qsort(elem_list, count, sizeof(*elem_list), bluealsa_elem_cmp);

	/* Detect element name duplicates and annotate them with the
	 * consecutive device ID number - make ALSA library happy. */
	if (!ctl->single_device)
		for (i = 0; i < count; i++) {

			char tmp[sizeof(elem_list[0].name)];
			bool duplicated = false;
			size_t ii;

			for (ii = i + 1; ii < count; ii++)
				if (strcmp(elem_list[i].name, elem_list[ii].name) == 0) {
					bluealsa_elem_set_name(&elem_list[ii], strcpy(tmp, elem_list[ii].dev->name),
							bluealsa_dev_get_id(ctl, elem_list[ii].pcm));
					duplicated = true;
				}

			if (duplicated)
				bluealsa_elem_set_name(&elem_list[i], strcpy(tmp, elem_list[i].dev->name),
						bluealsa_dev_get_id(ctl, elem_list[i].pcm));

		}

	ctl->elem_list = elem_list;
	ctl->elem_list_size = count;

	return count;
}

static void bluealsa_close(snd_ctl_ext_t *ext) {

	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;
	size_t i;

	bluealsa_dbus_connection_ctx_free(&ctl->dbus_ctx);

	if (ctl->pipefd[0] != -1)
		close(ctl->pipefd[0]);
	if (ctl->pipefd[1] != -1)
		close(ctl->pipefd[1]);

	for (i = 0; i < ctl->dev_list_size; i++)
		free(ctl->dev_list[i]);
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

	snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_id_set_name(id, ctl->elem_list[offset].name);

	return 0;
}

static snd_ctl_ext_key_t bluealsa_find_elem(snd_ctl_ext_t *ext, const snd_ctl_elem_id_t *id) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;

	unsigned int numid = snd_ctl_elem_id_get_numid(id);

	if (numid > 0 && numid <= ctl->elem_list_size)
		return numid - 1;

	const char *name = snd_ctl_elem_id_get_name(id);
	size_t i;

	for (i = 0; i < ctl->elem_list_size; i++)
		if (strcmp(ctl->elem_list[i].name, name) == 0)
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
	case CTL_ELEM_TYPE_SWITCH:
		return -EINVAL;
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
	}

	return 0;
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
		value[0] = active ? !pcm->volume.ch1_muted : 0;
		if (pcm->channels == 2)
			value[1] = active ? !pcm->volume.ch2_muted : 0;
		break;
	case CTL_ELEM_TYPE_VOLUME:
		value[0] = active ? pcm->volume.ch1_volume : 0;
		if (pcm->channels == 2)
			value[1] = active ? pcm->volume.ch2_volume : 0;
		break;
	}

	return 0;
}

static int bluealsa_write_integer(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, long *value) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;

	if (key > ctl->elem_list_size)
		return -EINVAL;

	struct ctl_elem *elem = &ctl->elem_list[key];
	struct ba_pcm *pcm = elem->pcm;
	uint16_t old = pcm->volume.raw;

	if (!elem->active) {
		/* Ignore the write request because the associated PCM profile has been
		 * disconnected. Create an update event so the application is informed
		 * that the value has been reset to zero. */
		bluealsa_event_elem_updated(ctl, elem->name);
		char ping = 1;
		write(ctl->pipefd[1], &ping, sizeof(ping));
		return 1;
	}

	switch (elem->type) {
	case CTL_ELEM_TYPE_BATTERY:
		/* this element should be read-only */
		return -EINVAL;
	case CTL_ELEM_TYPE_SWITCH:
		pcm->volume.ch1_muted = !value[0];
		if (pcm->channels == 2)
			pcm->volume.ch2_muted = !value[1];
		break;
	case CTL_ELEM_TYPE_VOLUME:
		pcm->volume.ch1_volume = value[0];
		if (pcm->channels == 2)
			pcm->volume.ch2_volume = value[1];
		break;
	}

	/* check whether update is required */
	if (pcm->volume.raw == old)
		return 0;

	if (!bluealsa_dbus_pcm_update(&ctl->dbus_ctx, pcm, BLUEALSA_PCM_VOLUME, NULL))
		return -ENOMEM;

	return 1;
}

static void bluealsa_subscribe_events(snd_ctl_ext_t *ext, int subscribe) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;

	if (subscribe) {
		bluealsa_dbus_connection_signal_match_add(&ctl->dbus_ctx, ctl->dbus_ctx.ba_service, NULL,
				DBUS_INTERFACE_OBJECT_MANAGER, "InterfacesAdded", "path_namespace='/org/bluealsa'");
		bluealsa_dbus_connection_signal_match_add(&ctl->dbus_ctx, ctl->dbus_ctx.ba_service, NULL,
				DBUS_INTERFACE_OBJECT_MANAGER, "InterfacesRemoved", "path_namespace='/org/bluealsa'");
		char dbus_args[50];
		snprintf(dbus_args, sizeof(dbus_args), "arg0='%s',arg2=''", ctl->dbus_ctx.ba_service);
		bluealsa_dbus_connection_signal_match_add(&ctl->dbus_ctx, DBUS_SERVICE_DBUS, NULL,
				DBUS_INTERFACE_DBUS, "NameOwnerChanged", dbus_args);
		bluealsa_dbus_connection_signal_match_add(&ctl->dbus_ctx, ctl->dbus_ctx.ba_service, NULL,
				DBUS_INTERFACE_PROPERTIES, "PropertiesChanged", "arg0='"BLUEALSA_INTERFACE_PCM"'");
		bluealsa_dbus_connection_signal_match_add(&ctl->dbus_ctx, ctl->dbus_ctx.ba_service, NULL,
				DBUS_INTERFACE_PROPERTIES, "PropertiesChanged", "arg0='"BLUEALSA_INTERFACE_RFCOMM"'");
		bluealsa_dbus_connection_signal_match_add(&ctl->dbus_ctx, "org.bluez", NULL,
				DBUS_INTERFACE_PROPERTIES, "PropertiesChanged", "arg0='org.bluez.Device1'");
	}
	else
		bluealsa_dbus_connection_signal_match_clean(&ctl->dbus_ctx);

	dbus_connection_flush(ctl->dbus_ctx.conn);
}

static dbus_bool_t bluealsa_dbus_msg_update_dev(const char *key,
		DBusMessageIter *variant, void *userdata, DBusError *error) {
	(void)error;

	struct bt_dev *dev = (struct bt_dev *)userdata;
	dev->mask = BT_DEV_MASK_NONE;

	if (strcmp(key, "Alias") == 0) {
		const char *alias;
		dbus_message_iter_get_basic(variant, &alias);
		*stpncpy(dev->name, alias, sizeof(dev->name) - 1) = '\0';
		dev->mask = BT_DEV_MASK_UPDATE;
	}
	else if (strcmp(key, "Battery") == 0) {
		signed char level;
		dbus_message_iter_get_basic(variant, &level);
		dev->mask = BT_DEV_MASK_UPDATE;
		if (dev->battery_level == -1)
			dev->mask = BT_DEV_MASK_ADD | BT_DEV_MASK_UPDATE;
		dev->battery_level = level;
	}
	else if (strcmp(key, "Connected") == 0) {
		dbus_bool_t connected;
		dbus_message_iter_get_basic(variant, &connected);
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
	size_t i;

	if (strcmp(interface, DBUS_INTERFACE_PROPERTIES) == 0 &&
			strcmp(signal, "PropertiesChanged") == 0) {

		const char *updated_interface;
		dbus_message_iter_get_basic(&iter, &updated_interface);
		dbus_message_iter_next(&iter);

		/* handle BlueZ device properties update */
		if (strcmp(updated_interface, "org.bluez.Device1") == 0)
			for (i = 0; i < ctl->elem_list_size; i++) {
				struct bt_dev *dev = ctl->elem_list[i].dev;
				if (strcmp(dev->device_path, path) == 0) {
					bluealsa_dbus_message_iter_dict(&iter, NULL,
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
			for (i = 0; i < ctl->elem_list_size; i++) {
				struct ctl_elem *elem = &ctl->elem_list[i];
				struct bt_dev *dev = elem->dev;
				if (strcmp(dev->rfcomm_path, path) == 0) {
					bluealsa_dbus_message_iter_dict(&iter, NULL,
							bluealsa_dbus_msg_update_dev, dev);
					/* for non-dynamic mode we need to use update logic */
					if (ctl->dynamic &&
							dev->mask & BT_DEV_MASK_ADD)
						goto remove_add;
					if (elem->type != CTL_ELEM_TYPE_BATTERY)
						continue;
					if (dev->mask & BT_DEV_MASK_UPDATE)
						bluealsa_event_elem_updated(ctl, elem->name);
				}
			}

		/* handle BlueALSA PCM properties update */
		if (strcmp(updated_interface, BLUEALSA_INTERFACE_PCM) == 0)
			for (i = 0; i < ctl->elem_list_size; i++) {
				struct ctl_elem *elem = &ctl->elem_list[i];
				struct ba_pcm *pcm = elem->pcm;
				if (elem->type == CTL_ELEM_TYPE_BATTERY)
					continue;
				if (strcmp(pcm->pcm_path, path) == 0) {
					bluealsa_dbus_message_iter_get_pcm_props(&iter, NULL, pcm);
					bluealsa_event_elem_updated(ctl, elem->name);
				}
			}

	}
	else if (strcmp(interface, DBUS_INTERFACE_OBJECT_MANAGER) == 0) {

		if (strcmp(signal, "InterfacesAdded") == 0) {
			struct ba_pcm pcm;
			if (bluealsa_dbus_message_iter_get_pcm(&iter, NULL, &pcm) &&
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

	for (i = 0; i < ctl->elem_list_size; i++)
		bluealsa_event_elem_removed(ctl, ctl->elem_list[i].name);

	bluealsa_create_elem_list(ctl);

	for (i = 0; i < ctl->elem_list_size; i++)
		bluealsa_event_elem_added(ctl, ctl->elem_list[i].name);

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

		snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
		snd_ctl_elem_id_set_name(id, ctl->elem_update_list[ctl->elem_update_event_i].name);
		*event_mask = ctl->elem_update_list[ctl->elem_update_event_i].event_mask;

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
	bluealsa_dbus_connection_dispatch(&ctl->dbus_ctx);
	/* For the same reason, we also need to clear any internal ping events. */
	if (ctl->single_device) {
		char buffer[16];
		read(ctl->pipefd[0], buffer, sizeof(buffer));
	}

	if (ctl->elem_update_list_size)
		return bluealsa_read_event(ext, id, event_mask);
	return -EAGAIN;
}

static int bluealsa_poll_descriptors_count(snd_ctl_ext_t *ext) {
	struct bluealsa_ctl *ctl = ext->private_data;

	nfds_t nfds = 0;
	bluealsa_dbus_connection_poll_fds(&ctl->dbus_ctx, NULL, &nfds);

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
		pfd[0].fd = ctl->pipefd[0];
		pfd[0].events = POLLIN;
		++pipe_nfds;
	}
	if (ctl->pipefd[1] > -1) {
		pfd[pipe_nfds].fd = ctl->pipefd[1];
		/* For the write end of our internal PIPE we are not interested
		 * in any I/O events, only in error condition. */
		pfd[pipe_nfds].events = 0;
		++pipe_nfds;
	}
	nfds_t dbus_nfds = nfds - pipe_nfds;

	if (!bluealsa_dbus_connection_poll_fds(&ctl->dbus_ctx, &pfd[pipe_nfds], &dbus_nfds))
		return -EINVAL;

	return pipe_nfds + dbus_nfds;
}

static int bluealsa_poll_revents(snd_ctl_ext_t *ext, struct pollfd *pfd,
		unsigned int nfds, unsigned short *revents) {
	struct bluealsa_ctl *ctl = ext->private_data;
	nfds_t pipe_nfds = 0;

	*revents = 0;

	if (ctl->pipefd[0] > -1) {
		if (pfd[0].revents) {
			char buffer[16];
			read(ctl->pipefd[0], buffer, sizeof(buffer));
		}
		*revents |= pfd[0].revents;
		++pipe_nfds;
	}
	if (ctl->pipefd[1] > -1) {
		*revents |= pfd[pipe_nfds].revents;
		++pipe_nfds;
	}

	if (bluealsa_dbus_connection_poll_dispatch(&ctl->dbus_ctx, &pfd[pipe_nfds], nfds - pipe_nfds))
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
	.read_integer = bluealsa_read_integer,
	.write_integer = bluealsa_write_integer,
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
	bool show_battery = false;
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
		if (strcmp(id, "battery") == 0) {
			if ((ret = snd_config_get_bool(n)) < 0) {
				SNDERR("Invalid type for %s", id);
				return -EINVAL;
			}
			show_battery = !!ret;
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

	ctl->show_battery = show_battery;
	ctl->single_device = single_device_mode;
	ctl->dynamic = dynamic;

	dbus_threads_init_default();

	if (!bluealsa_dbus_connection_ctx_init(&ctl->dbus_ctx, service, &err)) {
		SNDERR("Couldn't initialize D-Bus context: %s", err.message);
		ret = -ENOMEM;
		goto fail;
	}

	if (!dbus_connection_add_filter(ctl->dbus_ctx.conn, bluealsa_dbus_msg_filter, ctl, NULL)) {
		SNDERR("Couldn't add D-Bus filter: %s", strerror(ENOMEM));
		ret = -ENOMEM;
		goto fail;
	}

	if (!bluealsa_dbus_get_pcms(&ctl->dbus_ctx, &ctl->pcm_list, &ctl->pcm_list_size, &err)) {
		SNDERR("Couldn't get BlueALSA PCM list: %s", err.message);
		ret = -ENODEV;
		goto fail;
	}

	if (ctl->single_device) {
		const size_t pcm_list_size = ctl->pcm_list_size;
		struct ba_pcm *pcm_list = ctl->pcm_list;

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

			size_t i;
			for (i = 0; i < pcm_list_size; i++) {
				if (pcm_list[i].sequence >= seq) {
					seq = pcm_list[i].sequence;
					latest = &pcm_list[i];
				}
			}

			bacpy(&ba_addr, &latest->addr);

		}

		/* Filter the PCM list so that it contains only those
		 * PCMs belonging to the selected BT device. */
		size_t i, count;
		for (i = count = 0; i < pcm_list_size; i++)
			if (bacmp(&ba_addr, &pcm_list[i].addr) == 0)
				memmove(&pcm_list[count++], &pcm_list[i], sizeof(*pcm_list));
		ctl->pcm_list_size = count;

	}

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
	return ret;
}

SND_CTL_PLUGIN_SYMBOL(bluealsa)
