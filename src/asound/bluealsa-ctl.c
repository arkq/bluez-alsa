/*
 * bluealsa-ctl.c
 * Copyright (c) 2016-2018 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <alsa/asoundlib.h>
#include <alsa/control_external.h>

#include "shared/ctl-client.h"
#include "shared/ctl-proto.h"
#include "shared/log.h"


enum ctl_elem_type {
	CTL_ELEM_TYPE_BATTERY,
	CTL_ELEM_TYPE_SWITCH,
	CTL_ELEM_TYPE_VOLUME,
};

struct ctl_elem {
	enum ctl_elem_type type;
	struct ba_msg_device *device;
	struct ba_msg_transport *transport;
	char name[44 /* internal ALSA constraint */ + 1];
	/* if true, element is a playback control */
	bool playback;
};

struct ctl_elem_update {
	char name[sizeof(((struct ctl_elem *)0)->name)];
	unsigned int event_mask;
	struct ctl_elem *_elem;
};

struct bluealsa_ctl {
	snd_ctl_ext_t ext;

	/* bluealsa socket */
	int fd;

	/* if true, show battery meter */
	bool battery;

	/* list of all BT devices */
	struct ba_msg_device *devices;
	size_t devices_count;

	/* list of all transports */
	struct ba_msg_transport *transports;
	size_t transports_count;

	/* list of control elements */
	struct ctl_elem *elems;
	size_t elems_count;

	/* list of control element update events */
	struct ctl_elem_update *updates;
	size_t updates_count;

};


/**
 * Get device ID number.
 *
 * @param ctl An address to the bluealsa ctl structure.
 * @param device An address to the device structure.
 * @return The device ID number, or -1 upon error. */
static int bluealsa_get_device_id(const struct bluealsa_ctl *ctl,
		const struct ba_msg_device *device) {

	size_t i;

	for (i = 0; i < ctl->devices_count; i++)
		if (bacmp(&ctl->devices[i].addr, &device->addr) == 0)
			return i;

	return -1;
}

/**
 * Set element name within the element structure.
 *
 * @param elem An address to the element structure.
 * @param id Device ID number. If the ID is other than -1, it will be
 *   attached to the element name in order to prevent duplications. */
static void bluealsa_set_elem_name(struct ctl_elem *elem, int id) {

	const enum ctl_elem_type type = elem->type;
	const struct ba_msg_device *device = elem->device;
	const struct ba_msg_transport *transport = elem->transport;

	int len = sizeof(elem->name) - 16 - 1;
	char no[8] = "";

	if (id != -1) {
		sprintf(no, " #%d", id + 1);
		len -= strlen(no);
	}

	if (type == CTL_ELEM_TYPE_BATTERY) {
		len -= 10;
		while (isspace(device->name[len - 1]))
			len--;
		sprintf(elem->name, "%.*s%s | Battery", len, device->name, no);
	}
	else if (transport != NULL) {
		/* avoid name duplication by adding profile suffixes */
		switch (transport->type) {
		case BA_PCM_TYPE_NULL:
			break;
		case BA_PCM_TYPE_A2DP:
			len -= 7;
			while (isspace(device->name[len - 1]))
				len--;
			sprintf(elem->name, "%.*s%s - A2DP", len, device->name, no);
			break;
		case BA_PCM_TYPE_SCO:
			len -= 6;
			while (isspace(device->name[len - 1]))
				len--;
			sprintf(elem->name, "%.*s%s - SCO", len, device->name, no);
			break;
		}
	}

	/* ALSA library determines the element type by checking it's
	 * name suffix. This feature is not well documented, though. */

	strcat(elem->name, elem->playback ? " Playback" : " Capture");

	switch (type) {
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
 * Compare two control element structures.
 *
 * @param e1 Pointer to control element structure.
 * @param e2 Pointer to control element structure.
 * @return It returns an integer equal to, or greater than zero if e1 is
 *   found, respectively, to match, or be different than e2. */
static int bluealsa_ctl_elem_cmp(const struct ctl_elem *e1, const struct ctl_elem *e2) {

	const struct ba_msg_device *d1 = e1->device;
	const struct ba_msg_device *d2 = e2->device;
	const struct ba_msg_transport *t1 = e1->transport;
	const struct ba_msg_transport *t2 = e2->transport;
	bool updated = false;

	switch (e1->type) {
	case CTL_ELEM_TYPE_BATTERY:
		updated |= d1->battery_level != d2->battery_level;
		break;
	case CTL_ELEM_TYPE_SWITCH:
		switch (t1->type) {
		case BA_PCM_TYPE_NULL:
			return -EINVAL;
		case BA_PCM_TYPE_A2DP:
			updated |= t1->ch1_muted != t2->ch1_muted;
			if (t1->channels == 2)
				updated |= t1->ch2_muted != t2->ch2_muted;
			break;
		case BA_PCM_TYPE_SCO:
			if (e1->playback)
				updated |= t1->ch1_muted != t2->ch1_muted;
			else
				updated |= t1->ch2_muted != t2->ch2_muted;
			break;
		}
		break;
	case CTL_ELEM_TYPE_VOLUME:
		switch (t1->type) {
		case BA_PCM_TYPE_NULL:
			return -EINVAL;
		case BA_PCM_TYPE_A2DP:
			updated |= t1->ch1_volume != t2->ch1_volume;
			if (t1->channels == 2)
				updated |= t1->ch2_volume != t2->ch2_volume;
			break;
		case BA_PCM_TYPE_SCO:
			if (e1->playback)
				updated |= t1->ch1_volume != t2->ch1_volume;
			else
				updated |= t1->ch2_volume != t2->ch2_volume;
			break;
		}
		break;
	}

	return updated;
}

static int bluealsa_ctl_elem_update_cmp(const void *p1, const void *p2) {
	return strcmp(((struct ctl_elem_update *)p1)->name, ((struct ctl_elem_update *)p2)->name);
}

static void bluealsa_close(snd_ctl_ext_t *ext) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;
	close(ctl->fd);
	free(ctl->devices);
	free(ctl->transports);
	free(ctl->elems);
	free(ctl->updates);
	free(ctl);
}

static int bluealsa_elem_count(snd_ctl_ext_t *ext) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;

	size_t count = 0;
	ssize_t cnt;
	size_t i;

	if ((cnt = bluealsa_get_devices(ctl->fd, &ctl->devices)) == -1)
		return -errno;
	ctl->devices_count = cnt;

	if ((cnt = bluealsa_get_transports(ctl->fd, &ctl->transports)) == -1)
		return -errno;
	ctl->transports_count = cnt;

	for (i = 0; i < ctl->devices_count; i++) {
		/* add additional element for battery level */
		if (ctl->battery && ctl->devices[i].battery)
			count++;
	}

	for (i = 0; i < ctl->transports_count; i++) {
		/* Every stream has two controls associated to itself - volume adjustment
		 * and mute switch. A2DP transport contains only one stream. However, SCO
		 * transport represent both streams - playback and capture. */
		switch (ctl->transports[i].type) {
		case BA_PCM_TYPE_NULL:
			continue;
		case BA_PCM_TYPE_A2DP:
			count += 2;
			break;
		case BA_PCM_TYPE_SCO:
			count += 4;
			break;
		}
	}

	ctl->elems = malloc(sizeof(*ctl->elems) * count);
	ctl->elems_count = count;
	count = 0;

	/* construct control elements based on received transports */
	for (i = 0; i < ctl->transports_count; i++) {

		struct ba_msg_transport *transport = &ctl->transports[i];
		struct ba_msg_device *device = NULL;
		size_t ii;

		/* get device structure for given transport */
		for (ii = 0; ii < ctl->devices_count; ii++) {
			if (bacmp(&ctl->devices[ii].addr, &transport->addr) == 0) {
				device = &ctl->devices[ii];
				break;
			}
		}

		/* If the timing is right, the device list might not contain all devices.
		 * It will happen, when between the get devices and get transports calls
		 * a new BT device has been connected. */
		if (device == NULL)
			continue;

		switch (transport->type) {
		case BA_PCM_TYPE_NULL:
			break;

		case BA_PCM_TYPE_A2DP:

			ctl->elems[count].type = CTL_ELEM_TYPE_VOLUME;
			ctl->elems[count].device = device;
			ctl->elems[count].transport = transport;
			ctl->elems[count].playback = transport->stream == BA_PCM_STREAM_PLAYBACK;
			bluealsa_set_elem_name(&ctl->elems[count], -1);
			count++;

			ctl->elems[count].type = CTL_ELEM_TYPE_SWITCH;
			ctl->elems[count].device = device;
			ctl->elems[count].transport = transport;
			ctl->elems[count].playback = transport->stream == BA_PCM_STREAM_PLAYBACK;
			bluealsa_set_elem_name(&ctl->elems[count], -1);
			count++;

			break;

		case BA_PCM_TYPE_SCO:

			ctl->elems[count].type = CTL_ELEM_TYPE_VOLUME;
			ctl->elems[count].device = device;
			ctl->elems[count].transport = transport;
			ctl->elems[count].playback = true;
			bluealsa_set_elem_name(&ctl->elems[count], -1);
			count++;

			ctl->elems[count].type = CTL_ELEM_TYPE_SWITCH;
			ctl->elems[count].device = device;
			ctl->elems[count].transport = transport;
			ctl->elems[count].playback = true;
			bluealsa_set_elem_name(&ctl->elems[count], -1);
			count++;

			ctl->elems[count].type = CTL_ELEM_TYPE_VOLUME;
			ctl->elems[count].device = device;
			ctl->elems[count].transport = transport;
			ctl->elems[count].playback = false;
			bluealsa_set_elem_name(&ctl->elems[count], -1);
			count++;

			ctl->elems[count].type = CTL_ELEM_TYPE_SWITCH;
			ctl->elems[count].device = device;
			ctl->elems[count].transport = transport;
			ctl->elems[count].playback = false;
			bluealsa_set_elem_name(&ctl->elems[count], -1);
			count++;

			break;

		}

	}

	/* construct device specific elements */
	for (i = 0; i < ctl->devices_count; i++) {
		/* add additional element for battery level */
		if (ctl->battery && ctl->devices[i].battery) {
			ctl->elems[count].type = CTL_ELEM_TYPE_BATTERY;
			ctl->elems[count].device = &ctl->devices[i];
			ctl->elems[count].transport = NULL;
			ctl->elems[count].playback = true;
			bluealsa_set_elem_name(&ctl->elems[count], -1);
			count++;
		}
	}

	/* Detect element name duplicates and annotate them with the consecutive
	 * device ID number - which will make ALSA library happy. */
	for (i = 0; i < ctl->elems_count; i++) {

		bool duplicated = false;
		size_t ii;

		for (ii = i + 1; ii < ctl->elems_count; ii++)
			if (strcmp(ctl->elems[i].name, ctl->elems[ii].name) == 0) {
				bluealsa_set_elem_name(&ctl->elems[ii],
						bluealsa_get_device_id(ctl, ctl->elems[ii].device));
				duplicated = true;
			}

		if (duplicated)
			bluealsa_set_elem_name(&ctl->elems[i],
					bluealsa_get_device_id(ctl, ctl->elems[i].device));

	}

	return count;
}

static int bluealsa_elem_list(snd_ctl_ext_t *ext, unsigned int offset, snd_ctl_elem_id_t *id) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;

	if (offset > ctl->elems_count)
		return -EINVAL;

	snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_id_set_name(id, ctl->elems[offset].name);

	return 0;
}

static snd_ctl_ext_key_t bluealsa_find_elem(snd_ctl_ext_t *ext, const snd_ctl_elem_id_t *id) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;

	unsigned int numid = snd_ctl_elem_id_get_numid(id);

	if (numid > 0 && numid <= ctl->elems_count)
		return numid - 1;

	const char *name = snd_ctl_elem_id_get_name(id);
	size_t i;

	for (i = 0; i < ctl->elems_count; i++)
		if (strcmp(ctl->elems[i].name, name) == 0)
			return i;

	return SND_CTL_EXT_KEY_NOT_FOUND;
}

static int bluealsa_get_attribute(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key,
		int *type, unsigned int *acc, unsigned int *count) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;

	if (key > ctl->elems_count)
		return -EINVAL;

	const struct ctl_elem *elem = &ctl->elems[key];
	const struct ba_msg_transport *transport = elem->transport;

	switch (elem->type) {
	case CTL_ELEM_TYPE_BATTERY:
		*acc = SND_CTL_EXT_ACCESS_READ;
		*type = SND_CTL_ELEM_TYPE_INTEGER;
		*count = 1;
		break;
	case CTL_ELEM_TYPE_SWITCH:
		*acc = SND_CTL_EXT_ACCESS_READWRITE;
		*type = SND_CTL_ELEM_TYPE_BOOLEAN;
		*count = transport->channels;
		break;
	case CTL_ELEM_TYPE_VOLUME:
		*acc = SND_CTL_EXT_ACCESS_READWRITE;
		*type = SND_CTL_ELEM_TYPE_INTEGER;
		*count = transport->channels;
		break;
	}

	return 0;
}

static int bluealsa_get_integer_info(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key,
		long *imin, long *imax, long *istep) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;

	if (key > ctl->elems_count)
		return -EINVAL;

	const struct ctl_elem *elem = &ctl->elems[key];
	const struct ba_msg_transport *transport = elem->transport;

	switch (elem->type) {
	case CTL_ELEM_TYPE_BATTERY:
		*imin = 0;
		*imax = 100;
		*istep = 1;
		break;
	case CTL_ELEM_TYPE_SWITCH:
		return -EINVAL;
	case CTL_ELEM_TYPE_VOLUME:
		switch (transport->type) {
		case BA_PCM_TYPE_NULL:
			return -EINVAL;
		case BA_PCM_TYPE_A2DP:
			*imax = 127;
			break;
		case BA_PCM_TYPE_SCO:
			*imax = 15;
			break;
		}
		*imin = 0;
		*istep = 1;
		break;
	}

	return 0;
}

static int bluealsa_read_integer(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, long *value) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;

	if (key > ctl->elems_count)
		return -EINVAL;

	const struct ctl_elem *elem = &ctl->elems[key];
	const struct ba_msg_device *device = elem->device;
	const struct ba_msg_transport *transport = elem->transport;

	switch (elem->type) {
	case CTL_ELEM_TYPE_BATTERY:
		value[0] = device->battery_level;
		break;
	case CTL_ELEM_TYPE_SWITCH:
		switch (transport->type) {
		case BA_PCM_TYPE_NULL:
			return -EINVAL;
		case BA_PCM_TYPE_A2DP:
			value[0] = !transport->ch1_muted;
			if (transport->channels == 2)
				value[1] = !transport->ch2_muted;
			break;
		case BA_PCM_TYPE_SCO:
			if (elem->playback)
				value[0] = !transport->ch1_muted;
			else
				value[0] = !transport->ch2_muted;
			break;
		}
		break;
	case CTL_ELEM_TYPE_VOLUME:
		switch (transport->type) {
		case BA_PCM_TYPE_NULL:
			return -EINVAL;
		case BA_PCM_TYPE_A2DP:
			value[0] = transport->ch1_volume;
			if (transport->channels == 2)
				value[1] = transport->ch2_volume;
			break;
		case BA_PCM_TYPE_SCO:
			if (elem->playback)
				value[0] = transport->ch1_volume;
			else
				value[0] = transport->ch2_volume;
			break;
		}
		break;
	}

	return 0;
}

static int bluealsa_write_integer(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, long *value) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;

	if (key > ctl->elems_count)
		return -EINVAL;

	struct ctl_elem *elem = &ctl->elems[key];
	struct ba_msg_transport *transport = elem->transport;

	if (transport == NULL)
		return -EINVAL;

	switch (elem->type) {
	case CTL_ELEM_TYPE_BATTERY:
		/* this element should be read-only */
		return -EINVAL;
	case CTL_ELEM_TYPE_SWITCH:
		switch (transport->type) {
		case BA_PCM_TYPE_NULL:
			return -EINVAL;
		case BA_PCM_TYPE_A2DP:
			transport->ch1_muted = !value[0];
			if (transport->channels == 2)
				transport->ch2_muted = !value[1];
			break;
		case BA_PCM_TYPE_SCO:
			if (elem->playback)
				transport->ch1_muted = !value[0];
			else
				transport->ch2_muted = !value[0];
			break;
		}
		break;
	case CTL_ELEM_TYPE_VOLUME:
		switch (transport->type) {
		case BA_PCM_TYPE_NULL:
			return -EINVAL;
		case BA_PCM_TYPE_A2DP:
			transport->ch1_volume = value[0];
			if (transport->channels == 2)
				transport->ch2_volume = value[1];
			break;
		case BA_PCM_TYPE_SCO:
			if (elem->playback)
				transport->ch1_volume = value[0];
			else
				transport->ch2_volume = value[0];
			break;
		}
		break;
	}

	if (bluealsa_set_transport_volume(ctl->fd, transport,
				transport->ch1_muted, transport->ch1_volume,
				transport->ch2_muted, transport->ch2_volume) == -1)
		return -errno;
	return 0;
}

static void bluealsa_subscribe_events(snd_ctl_ext_t *ext, int subscribe) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;
	if (bluealsa_subscribe(ctl->fd, subscribe ? 0xFFFF : 0) == -1)
		SNDERR("BlueALSA subscription failed: %s", strerror(errno));
}

static int bluealsa_read_event(snd_ctl_ext_t *ext, snd_ctl_elem_id_t *id, unsigned int *event_mask) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;

	if (ctl->updates_count) {
		ctl->updates_count--;

		snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
		snd_ctl_elem_id_set_name(id, ctl->updates[ctl->updates_count].name);
		*event_mask = ctl->updates[ctl->updates_count].event_mask;

		if (ctl->updates_count == 0) {
			free(ctl->updates);
			ctl->updates = NULL;
		}

		return 1;
	}

	struct ba_msg_event event;
	ssize_t ret;

	/* This code reads events from the socket until the EAGAIN is returned.
	 * Since EAGAIN is returned when operation would block (there is no more
	 * data to read), we are compliant with the ALSA specification. */
	while ((ret = recv(ctl->fd, &event, sizeof(event), MSG_DONTWAIT)) == -1 && errno == EINTR)
		continue;
	if (ret == -1)
		return -errno;

	/* Upon server disconnection, our socket file descriptor is in the ready for
	 * reading state. However, there is no data to be read. In such a case, we
	 * have to indicate, that the controller has been unplugged. Since, it is not
	 * possible to manipulate revents - snd_mixer_poll_descriptors_revents() does
	 * not call poll_revents() callback - we are going to close our socket, which
	 * (at least for alsamixer) seems to do the trick. Anyhow, there is a caveat.
	 * If some other thread opens new file descriptor... we are doomed. */
	if (ret == 0) {
		close(ext->poll_fd);
		return -ENODEV;
	}

	/* Save current control elements for later usage. The call to the
	 * bluealsa_elem_count() will overwrite these pointers. */
	struct ba_msg_device *devices = ctl->devices;
	struct ba_msg_transport *transports = ctl->transports;
	struct ctl_elem *elems = ctl->elems;
	size_t count = ctl->elems_count;

	if ((ret = bluealsa_elem_count(ext)) < 0)
		return ret;

	/* This part is kinda tricky, however not very complicated. We are going
	 * to allocate buffer, which will store references to our previous control
	 * elements and to the new ones. Then, we are going to sort these elements
	 * alphabetically by name - name acts as a unique identifier. Finally, we
	 * are going to compare adjacent elements, and if names do match (the same
	 * element), we will check if such an element should be marked for update.
	 * Otherwise, element is added or removed. */

	ctl->updates_count = count + ctl->elems_count;
	ctl->updates = malloc(sizeof(*ctl->updates) * ctl->updates_count);

	struct ctl_elem_update *_tmp = ctl->updates;
	size_t i;

	for (i = 0; i < count; i++, _tmp++) {
		strcpy(_tmp->name, elems[i].name);
		_tmp->event_mask = SND_CTL_EVENT_MASK_REMOVE;
		_tmp->_elem = &elems[i];
	}
	for (i = 0; i < ctl->elems_count; i++, _tmp++) {
		strcpy(_tmp->name, ctl->elems[i].name);
		_tmp->event_mask = SND_CTL_EVENT_MASK_ADD;
		_tmp->_elem = &ctl->elems[i];
	}

	qsort(ctl->updates, ctl->updates_count, sizeof(*ctl->updates), bluealsa_ctl_elem_update_cmp);

	for (i = 0; i + 1 < ctl->updates_count; i++)
		if (strcmp(ctl->updates[i].name, ctl->updates[i + 1].name) == 0) {
			ctl->updates[i].event_mask = ctl->updates[i + 1].event_mask = 0;
			if (bluealsa_ctl_elem_cmp(ctl->updates[i]._elem, ctl->updates[i + 1]._elem) != 0)
				ctl->updates[i].event_mask = SND_CTL_EVENT_MASK_VALUE;
			i++;
		}

	free(devices);
	free(transports);
	free(elems);

	return bluealsa_read_event(ext, id, event_mask);
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
};

SND_CTL_PLUGIN_DEFINE_FUNC(bluealsa) {
	(void)root;

	snd_config_iterator_t i, next;
	const char *interface = "hci0";
	const char *battery = "no";
	struct bluealsa_ctl *ctl;
	int ret;

	snd_config_for_each(i, next, conf) {
		snd_config_t *n = snd_config_iterator_entry(i);

		const char *id;
		if (snd_config_get_id(n, &id) < 0)
			continue;

		if (strcmp(id, "comment") == 0 ||
				strcmp(id, "type") == 0 ||
				strcmp(id, "hint") == 0)
			continue;

		if (strcmp(id, "interface") == 0) {
			if (snd_config_get_string(n, &interface) < 0) {
				SNDERR("Invalid type for %s", id);
				return -EINVAL;
			}
			continue;
		}
		if (strcmp(id, "battery") == 0) {
			if (snd_config_get_string(n, &battery) < 0) {
				SNDERR("Invalid type for %s", id);
				return -EINVAL;
			}
			continue;
		}

		SNDERR("Unknown field %s", id);
		return -EINVAL;
	}

	if ((ctl = calloc(1, sizeof(*ctl))) == NULL)
		return -ENOMEM;

	if ((ctl->fd = bluealsa_open(interface)) == -1) {
		SNDERR("BlueALSA connection failed: %s", strerror(errno));
		ret = -errno;
		goto fail;
	}

	ctl->ext.version = SND_CTL_EXT_VERSION;
	ctl->ext.card_idx = 0;
	strncpy(ctl->ext.id, "bluealsa", sizeof(ctl->ext.id) - 1);
	strncpy(ctl->ext.driver, "BlueALSA", sizeof(ctl->ext.driver) - 1);
	strncpy(ctl->ext.name, "BlueALSA", sizeof(ctl->ext.name) - 1);
	strncpy(ctl->ext.longname, "Bluetooth Audio Hub Controller", sizeof(ctl->ext.longname) - 1);
	strncpy(ctl->ext.mixername, "BlueALSA Plugin", sizeof(ctl->ext.mixername) - 1);
	ctl->battery = strcmp(battery, "yes") == 0;

	ctl->ext.callback = &bluealsa_snd_ctl_ext_callback;
	ctl->ext.private_data = ctl;
	ctl->ext.poll_fd = ctl->fd;

	if ((ret = snd_ctl_ext_create(&ctl->ext, name, mode)) < 0)
		goto fail;

	*handlep = ctl->ext.handle;
	return 0;

fail:
	if (ctl->fd != -1)
		close(ctl->fd);
	free(ctl);
	return ret;
}

SND_CTL_PLUGIN_SYMBOL(bluealsa);
