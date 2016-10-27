/*
 * bluealsa-ctl.c
 * Copyright (c) 2016 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

#include <alsa/asoundlib.h>
#include <alsa/control_external.h>

#include "ctl.h"
#include "transport.h"
#include "log.c"


enum ctl_elem_type {
	CTL_ELEM_TYPE_BATTERY,
	CTL_ELEM_TYPE_SWITCH,
	CTL_ELEM_TYPE_VOLUME,
};

struct ctl_elem {
	enum ctl_elem_type type;
	struct msg_device *device;
	struct msg_transport *transport;
	/* if TRUE, element is a playback control */
	uint8_t playback;
};

struct bluealsa_ctl {
	snd_ctl_ext_t ext;

	/* bluealsa socket */
	int fd;

	/* list of all BT devices */
	struct msg_device *devices;
	size_t devices_count;

	/* list of all transports */
	struct msg_transport *transports;
	size_t transports_count;

	/* list of control elements */
	struct ctl_elem *elems;
	size_t elems_count;

};


/**
 * Get Bluetooth devices.
 *
 * @param ctl An address to the bluealsa ctl structure.
 * @return Upon success this function returns 0. Otherwise, -1 is returned. */
static int bluealsa_get_devices(struct bluealsa_ctl *ctl) {

	const struct request req = { .command = COMMAND_LIST_DEVICES };
	struct msg_device *devices = NULL;
	struct msg_device device;
	size_t i = 0;

	if (send(ctl->fd, &req, sizeof(req), MSG_NOSIGNAL) == -1)
		return -1;

	while (recv(ctl->fd, &device, sizeof(device), 0) == sizeof(device)) {
		devices = realloc(devices, (i + 1) * sizeof(*devices));
		memcpy(&devices[i], &device, sizeof(*devices));
		i++;
	}

	ctl->devices = devices;
	ctl->devices_count = i;
	return 0;
}

/**
 * Get PCM transports.
 *
 * @param ctl An address to the bluealsa ctl structure.
 * @return Upon success this function returns 0. Otherwise, -1 is returned. */
static int bluealsa_get_transports(struct bluealsa_ctl *ctl) {

	const struct request req = { .command = COMMAND_LIST_TRANSPORTS };
	struct msg_transport *transports = NULL;
	struct msg_transport transport;
	size_t i = 0;

	if (send(ctl->fd, &req, sizeof(req), MSG_NOSIGNAL) == -1)
		return -1;

	while (recv(ctl->fd, &transport, sizeof(transport), 0) == sizeof(transport)) {
		transports = realloc(transports, (i + 1) * sizeof(*transports));
		memcpy(&transports[i], &transport, sizeof(*transports));
		i++;
	}

	ctl->transports = transports;
	ctl->transports_count = i;
	return 0;
}

static void bluealsa_close(snd_ctl_ext_t *ext) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;
	close(ctl->fd);
	free(ctl->devices);
	free(ctl->transports);
	free(ctl->elems);
	free(ctl);
}

static int bluealsa_elem_count(snd_ctl_ext_t *ext) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;

	size_t count = 0;
	size_t i;

	if (bluealsa_get_devices(ctl) == -1)
		return -errno;
	if (bluealsa_get_transports(ctl) == -1)
		return -errno;

	for (i = 0; i < ctl->devices_count; i++) {
		/* add additional element for battery level */
		if (ctl->devices[i].battery)
			count++;
	}

	for (i = 0; i < ctl->transports_count; i++) {
		/* Every stream has two controls associated to itself - volume adjustment
		 * and mute switch. A2DP transport contains only one stream. However, HSP
		 * and HFP transports represent both streams - playback and capture. */
		switch (ctl->transports[i].profile) {
		case TRANSPORT_PROFILE_A2DP_SOURCE:
		case TRANSPORT_PROFILE_A2DP_SINK:
			count += 2;
			break;
		case TRANSPORT_PROFILE_HSP_HS:
		case TRANSPORT_PROFILE_HSP_AG:
		case TRANSPORT_PROFILE_HFP_HF:
		case TRANSPORT_PROFILE_HFP_AG:
			count += 4;
			break;
		default:
			SNDERR("Unsupported transport profile: %x", ctl->transports[i].profile);
			return -ENOTSUP;
		}
	}

	ctl->elems = malloc(sizeof(*ctl->elems) * count);
	ctl->elems_count = count;
	count = 0;

	/* construct control elements based on received transports */
	for (i = 0; i < ctl->transports_count; i++) {

		struct msg_transport *transport = &ctl->transports[i];
		struct msg_device *device = NULL;
		uint8_t profile;
		size_t ii;

		/* get device structure for given transport */
		for (ii = 0; ii < ctl->devices_count; ii++) {
			if (bacmp(&ctl->devices[ii].addr, &transport->addr) == 0)
				device = &ctl->devices[ii];
		}

		/* If the timing is right, the device list might not contain all devices.
		 * It will happen, when between the get devices and get transports calls,
		 * new BT device has been connected. */
		if (device == NULL)
			continue;

		switch (profile = transport->profile) {
		case TRANSPORT_PROFILE_A2DP_SOURCE:
		case TRANSPORT_PROFILE_A2DP_SINK:

			ctl->elems[count].type = CTL_ELEM_TYPE_VOLUME;
			ctl->elems[count].device = device;
			ctl->elems[count].transport = transport;
			ctl->elems[count].playback = profile == TRANSPORT_PROFILE_A2DP_SOURCE;
			count++;

			ctl->elems[count].type = CTL_ELEM_TYPE_SWITCH;
			ctl->elems[count].device = device;
			ctl->elems[count].transport = transport;
			ctl->elems[count].playback = profile == TRANSPORT_PROFILE_A2DP_SOURCE;
			count++;

			break;

		case TRANSPORT_PROFILE_HSP_HS:
		case TRANSPORT_PROFILE_HSP_AG:
		case TRANSPORT_PROFILE_HFP_HF:
		case TRANSPORT_PROFILE_HFP_AG:

			ctl->elems[count].type = CTL_ELEM_TYPE_VOLUME;
			ctl->elems[count].device = device;
			ctl->elems[count].transport = transport;
			ctl->elems[count].playback = TRUE;
			count++;

			ctl->elems[count].type = CTL_ELEM_TYPE_SWITCH;
			ctl->elems[count].device = device;
			ctl->elems[count].transport = transport;
			ctl->elems[count].playback = TRUE;
			count++;

			ctl->elems[count].type = CTL_ELEM_TYPE_VOLUME;
			ctl->elems[count].device = device;
			ctl->elems[count].transport = transport;
			ctl->elems[count].playback = FALSE;
			count++;

			ctl->elems[count].type = CTL_ELEM_TYPE_SWITCH;
			ctl->elems[count].device = device;
			ctl->elems[count].transport = transport;
			ctl->elems[count].playback = FALSE;
			count++;

			break;

		}

	}

	/* construct device specific elements */
	for (i = 0; i < ctl->devices_count; i++) {
		/* add additional element for battery level */
		if (ctl->devices[i].battery) {
			ctl->elems[count].type = CTL_ELEM_TYPE_BATTERY;
			ctl->elems[count].device = &ctl->devices[i];
			ctl->elems[count].transport = NULL;
			ctl->elems[count].playback = TRUE;
			count++;
		}
	}

	return count;
}

static int bluealsa_elem_list(snd_ctl_ext_t *ext, unsigned int offset, snd_ctl_elem_id_t *id) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;

	if (offset > ctl->elems_count)
		return -EINVAL;

	const struct ctl_elem *elem = &ctl->elems[offset];
	const struct msg_device *device = elem->device;
	const struct msg_transport *transport = elem->transport;
	char name[44 /* internal ALSA constraint */];

	strcpy(name, device->name);

	if (elem->type == CTL_ELEM_TYPE_BATTERY) {
		name[sizeof(name) - 10 - 16 - 1] = '\0';
		strcat(name, " | Battery");
	}

	if (transport != NULL) {
		/* avoid name duplication by adding profile suffixes */
		switch (transport->profile) {
		case TRANSPORT_PROFILE_A2DP_SOURCE:
		case TRANSPORT_PROFILE_A2DP_SINK:
			name[sizeof(name) - 7 - 16 - 1] = '\0';
			strcat(name, " - A2DP");
			break;
		case TRANSPORT_PROFILE_HSP_HS:
		case TRANSPORT_PROFILE_HSP_AG:
			name[sizeof(name) - 6 - 16 - 1] = '\0';
			strcat(name, " - HSP");
			break;
		case TRANSPORT_PROFILE_HFP_HF:
		case TRANSPORT_PROFILE_HFP_AG:
			name[sizeof(name) - 6 - 16 - 1] = '\0';
			strcat(name, " - HFP");
			break;
		}
	}

	/* XXX: It seems, that ALSA determines the element type by checking it's
	 *      name suffix. However, this functionality is not documented! */

	strcat(name, elem->playback ? " Playback" : " Capture");

	switch (elem->type) {
	case CTL_ELEM_TYPE_SWITCH:
		strcat(name, " Switch");
		break;
	case CTL_ELEM_TYPE_BATTERY:
	case CTL_ELEM_TYPE_VOLUME:
		strcat(name, " Volume");
		break;
	}

	snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_id_set_name(id, name);

	return 0;
}

static snd_ctl_ext_key_t bluealsa_find_elem(snd_ctl_ext_t *ext, const snd_ctl_elem_id_t *id) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;

	unsigned int numid = snd_ctl_elem_id_get_numid(id);

	if (numid > 0 && numid <= ctl->elems_count)
		return numid - 1;

	return SND_CTL_EXT_KEY_NOT_FOUND;
}

static int bluealsa_get_attribute(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key,
		int *type, unsigned int *acc, unsigned int *count) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;

	if (key > ctl->elems_count)
		return -EINVAL;

	const struct ctl_elem *elem = &ctl->elems[key];
	const struct msg_transport *transport = elem->transport;

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

	switch (elem->type) {
	case CTL_ELEM_TYPE_BATTERY:
		*imin = 0;
		*imax = 9;
		*istep = 1;
		break;
	case CTL_ELEM_TYPE_SWITCH:
		return -EINVAL;
	case CTL_ELEM_TYPE_VOLUME:
		*imin = 0;
		*imax = 127;
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
	const struct msg_device *device = elem->device;
	const struct msg_transport *transport = elem->transport;

	switch (elem->type) {
	case CTL_ELEM_TYPE_BATTERY:
		value[0] = device->battery_level;
		break;
	case CTL_ELEM_TYPE_SWITCH:
		value[0] = !transport->ch1_muted;
		if (transport->channels == 2)
			value[1] = !transport->ch2_muted;
		break;
	case CTL_ELEM_TYPE_VOLUME:
		value[0] = transport->ch1_volume;
		if (transport->channels == 2)
			value[1] = transport->ch2_volume;
		break;
	}

	return 0;
}

static int bluealsa_write_integer(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, long *value) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;

	if (key > ctl->elems_count)
		return -EINVAL;

	struct msg_status status;
	struct ctl_elem *elem = &ctl->elems[key];
	struct msg_transport *transport = elem->transport;

	if (transport == NULL)
		return -EINVAL;

	struct request req = {
		.command = COMMAND_TRANSPORT_SET_VOLUME,
		.addr = transport->addr,
		.profile = transport->profile,
		.ch1_muted = transport->ch1_muted,
		.ch1_volume = transport->ch1_volume,
		.ch2_muted = transport->ch2_muted,
		.ch2_volume = transport->ch2_volume,
	};

	switch (elem->type) {
	case CTL_ELEM_TYPE_BATTERY:
		/* this element should be read-only */
		return -EINVAL;
	case CTL_ELEM_TYPE_SWITCH:
		req.ch1_muted = transport->ch1_muted = !value[0];
		if (transport->channels == 2)
			req.ch2_muted = transport->ch2_muted = !value[1];
		break;
	case CTL_ELEM_TYPE_VOLUME:
		req.ch1_volume = transport->ch1_volume = value[0];
		if (transport->channels == 2)
			req.ch2_volume = transport->ch2_volume = value[1];
		break;
	}

	send(ctl->fd, &req, sizeof(req), MSG_NOSIGNAL);
	if (read(ctl->fd, &status, sizeof(status)) == -1)
		return -EIO;

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
};

SND_CTL_PLUGIN_DEFINE_FUNC(bluealsa) {
	(void)root;

	snd_config_iterator_t i, next;
	const char *interface = "hci0";
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

		SNDERR("Unknown field %s", id);
		return -EINVAL;
	}

	if ((ctl = calloc(1, sizeof(*ctl))) == NULL)
		return -ENOMEM;

	struct sockaddr_un saddr = { .sun_family = AF_UNIX };
	snprintf(saddr.sun_path, sizeof(saddr.sun_path) - 1,
			BLUEALSA_RUN_STATE_DIR "/%s", interface);

	if ((ctl->fd = socket(PF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) == -1) {
		ret = -errno;
		goto fail;
	}

	if (connect(ctl->fd, (struct sockaddr *)(&saddr), sizeof(saddr)) == -1) {
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

	ctl->ext.callback = &bluealsa_snd_ctl_ext_callback;
	ctl->ext.private_data = ctl;
	ctl->ext.poll_fd = -1;

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
