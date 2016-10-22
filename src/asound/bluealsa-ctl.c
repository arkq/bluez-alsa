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
	CTL_ELEM_TYPE_SWITCH,
	CTL_ELEM_TYPE_VOLUME,
};

struct ctl_elem {
	struct msg_transport *transport;
	snd_pcm_stream_t stream;
	enum ctl_elem_type type;
};

struct bluealsa_ctl {
	snd_ctl_ext_t ext;

	/* bluealsa socket */
	int fd;

	/* list of all transports */
	struct msg_transport *transports;
	size_t transports_count;

	/* list of control elements */
	struct ctl_elem *elems;
	size_t elems_count;

};


static void bluealsa_close(snd_ctl_ext_t *ext) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;
	close(ctl->fd);
	free(ctl->transports);
	free(ctl->elems);
	free(ctl);
}

static int bluealsa_elem_count(snd_ctl_ext_t *ext) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;

	const struct request req = {
		.command = COMMAND_LIST_TRANSPORTS,
	};

	send(ctl->fd, &req, sizeof(req), MSG_NOSIGNAL);

	struct msg_transport transport;
	size_t count = 0;
	size_t i = 0;

	while (recv(ctl->fd, &transport, sizeof(transport), 0) == sizeof(transport)) {

		ctl->transports = realloc(ctl->transports, (i + 1) * sizeof(*ctl->transports));
		memcpy(&ctl->transports[i], &transport, sizeof(*ctl->transports));
		i++;

		/* Every stream has two controls associated to itself - volume adjustment
		 * and mute switch. A2DP transport contains only one stream. However, HSP
		 * and HFP transports represent both streams - playback and capture. */
		switch (transport.profile) {
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
			SNDERR("Unsupported transport profile: %x", transport.profile);
			return -ENOTSUP;
		}

	}

	ctl->transports_count = i;
	ctl->elems = malloc(sizeof(*ctl->elems) * count);
	ctl->elems_count = count;

	/* construct control elements based on received transports */
	for (i = count = 0; i < ctl->transports_count; i++) {
		const uint8_t profile = ctl->transports[i].profile;
		switch (profile) {
		case TRANSPORT_PROFILE_A2DP_SOURCE:
		case TRANSPORT_PROFILE_A2DP_SINK:

			ctl->elems[count].transport = &ctl->transports[i];
			ctl->elems[count].stream = profile == TRANSPORT_PROFILE_A2DP_SOURCE
				? SND_PCM_STREAM_PLAYBACK : SND_PCM_STREAM_CAPTURE;
			ctl->elems[count].type = CTL_ELEM_TYPE_VOLUME;
			count++;

			ctl->elems[count].transport = &ctl->transports[i];
			ctl->elems[count].stream = profile == TRANSPORT_PROFILE_A2DP_SOURCE
				? SND_PCM_STREAM_PLAYBACK : SND_PCM_STREAM_CAPTURE;
			ctl->elems[count].type = CTL_ELEM_TYPE_SWITCH;
			count++;

			break;

		case TRANSPORT_PROFILE_HSP_HS:
		case TRANSPORT_PROFILE_HSP_AG:
		case TRANSPORT_PROFILE_HFP_HF:
		case TRANSPORT_PROFILE_HFP_AG:

			ctl->elems[count].transport = &ctl->transports[i];
			ctl->elems[count].stream = SND_PCM_STREAM_PLAYBACK;
			ctl->elems[count].type = CTL_ELEM_TYPE_VOLUME;
			count++;

			ctl->elems[count].transport = &ctl->transports[i];
			ctl->elems[count].stream = SND_PCM_STREAM_PLAYBACK;
			ctl->elems[count].type = CTL_ELEM_TYPE_SWITCH;
			count++;

			ctl->elems[count].transport = &ctl->transports[i];
			ctl->elems[count].stream = SND_PCM_STREAM_CAPTURE;
			ctl->elems[count].type = CTL_ELEM_TYPE_VOLUME;
			count++;

			ctl->elems[count].transport = &ctl->transports[i];
			ctl->elems[count].stream = SND_PCM_STREAM_CAPTURE;
			ctl->elems[count].type = CTL_ELEM_TYPE_SWITCH;
			count++;

			break;

		}
	}

	return count;
}

static int bluealsa_elem_list(snd_ctl_ext_t *ext, unsigned int offset, snd_ctl_elem_id_t *id) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;

	if (offset > ctl->elems_count)
		return -EINVAL;

	const struct ctl_elem *elem = &ctl->elems[offset];
	const struct msg_transport *transport = elem->transport;
	char name[sizeof(transport->name) + 25];

	strcpy(name, transport->name);

	/* avoid name duplication by adding profile suffixes */
	switch (transport->profile) {
	case TRANSPORT_PROFILE_A2DP_SOURCE:
	case TRANSPORT_PROFILE_A2DP_SINK:
		strcat(name, " (A2DP)");
		break;
	case TRANSPORT_PROFILE_HSP_HS:
	case TRANSPORT_PROFILE_HSP_AG:
		strcat(name, " (HSP)");
		break;
	case TRANSPORT_PROFILE_HFP_HF:
	case TRANSPORT_PROFILE_HFP_AG:
		strcat(name, " (HFP)");
		break;
	}

	/* XXX: It seems, that ALSA determines the element type by checking it's
	 *      name suffix. However, this functionality is not documented! */
	switch (elem->stream) {
	case SND_PCM_STREAM_PLAYBACK:
		strcat(name, " Playback");
		break;
	case SND_PCM_STREAM_CAPTURE:
		strcat(name, " Capture");
		break;
	}
	switch (elem->type) {
	case CTL_ELEM_TYPE_SWITCH:
		strcat(name, " Switch");
		break;
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

	*acc = SND_CTL_EXT_ACCESS_READWRITE;

	switch (elem->type) {
	case CTL_ELEM_TYPE_SWITCH:
		*type = SND_CTL_ELEM_TYPE_BOOLEAN;
		*count = 1;
		break;
	case CTL_ELEM_TYPE_VOLUME:
		*type = SND_CTL_ELEM_TYPE_INTEGER;
		*count = transport->channels;
		break;
	}

	return 0;
}

static int bluealsa_get_integer_info(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key,
		long *imin, long *imax, long *istep) {
	(void)ext;
	(void)key;
	*imin = 0;
	*imax = 100;
	*istep = 10;
	return 0;
}

static int bluealsa_read_integer(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, long *value) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;

	if (key > ctl->elems_count)
		return -EINVAL;

	const struct ctl_elem *elem = &ctl->elems[key];
	const struct msg_transport *transport = elem->transport;
	uint8_t channel = transport->channels;

	switch (elem->type) {
	case CTL_ELEM_TYPE_SWITCH:
		*value = !transport->muted;
		break;
	case CTL_ELEM_TYPE_VOLUME:
		while (channel--)
			value[channel] = transport->volume;
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
	struct request req = {
		.command = COMMAND_TRANSPORT_SET_VOLUME,
		.addr = transport->addr,
		.profile = transport->profile,
		.muted = transport->muted,
		.volume = transport->volume,
	};

	switch (elem->type) {
	case CTL_ELEM_TYPE_SWITCH:
		req.muted = transport->muted = !*value;
		break;
	case CTL_ELEM_TYPE_VOLUME: {

		int volume = 0;
		int i;

		for (i = 0; i < transport->channels; i++)
			volume += value[i];

		req.volume = transport->volume = volume / transport->channels;

		break;
	}}

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
