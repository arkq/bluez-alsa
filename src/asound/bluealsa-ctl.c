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


struct bluealsa_ctl {
	snd_ctl_ext_t ext;

	/* bluealsa socket */
	int fd;

	/* list of all currently available transports */
	struct msg_transport *transports;
	unsigned int transports_count;

};


static void bluealsa_close(snd_ctl_ext_t *ext) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;
	close(ctl->fd);
	free(ctl->transports);
	free(ctl);
}

static int bluealsa_elem_count(snd_ctl_ext_t *ext) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;

	const struct request req = {
		.command = COMMAND_LIST_TRANSPORTS,
	};

	send(ctl->fd, &req, sizeof(req), MSG_NOSIGNAL);

	struct msg_transport transport;
	int i = 0;

	while (recv(ctl->fd, &transport, sizeof(transport), 0) == sizeof(transport)) {
		ctl->transports = realloc(ctl->transports, (i + 1) * sizeof(*ctl->transports));
		memcpy(&ctl->transports[i], &transport, sizeof(*ctl->transports));
		i++;
	}

	ctl->transports_count = i;

	/* XXX: Every transport has two controls associated to itself - volume
	 *      adjustment and mute switch. Since ALSA operates on raw controls,
	 *      we need to multiply our transport count by 2. By convention we
	 *      will assume, that every even control is a volume regulator, and
	 *      every odd control is a mute switch. */
	return i * 2;
}

static int bluealsa_elem_list(snd_ctl_ext_t *ext, unsigned int offset, snd_ctl_elem_id_t *id) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;

	if (offset / 2 > ctl->transports_count)
		return -EINVAL;

	struct msg_transport *transport = &ctl->transports[offset / 2];
	char name[sizeof(transport->name) + 16];

	strcpy(name, transport->name);

	/* XXX: It seems, that ALSA determines the element type by checking it's
	 *      name suffix. However, this functionality is not documented! */
	switch (transport->profile) {
	case TRANSPORT_PROFILE_A2DP_SOURCE:
		strcat(name, offset % 2 ? " Playback Switch" : " Playback Volume");
		break;
	case TRANSPORT_PROFILE_A2DP_SINK:
		strcat(name, offset % 2 ? " Capture Switch" : " Capture Volume");
		break;
	}

	snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_id_set_name(id, name);

	return 0;
}

static snd_ctl_ext_key_t bluealsa_find_elem(snd_ctl_ext_t *ext, const snd_ctl_elem_id_t *id) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;

	unsigned int count = ctl->transports_count;
	unsigned int numid;

	numid = snd_ctl_elem_id_get_numid(id);
	if (numid > 0 && numid / 2 <= count)
		return numid - 1;

	return SND_CTL_EXT_KEY_NOT_FOUND;
}

static int bluealsa_get_attribute(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key,
		int *type, unsigned int *acc, unsigned int *count) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;

	if (key / 2 > ctl->transports_count)
		return -EINVAL;

	struct msg_transport *transport = &ctl->transports[key / 2];

	*acc = SND_CTL_EXT_ACCESS_READWRITE;

	if (key % 2) {
		*type = SND_CTL_ELEM_TYPE_BOOLEAN;
		*count = 1;
	}
	else {
		*type = SND_CTL_ELEM_TYPE_INTEGER;
		*count = transport->channels;
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

	if (key / 2 > ctl->transports_count)
		return -EINVAL;

	struct msg_transport *transport = &ctl->transports[key / 2];

	if (key % 2)
		*value = !transport->muted;
	else
		value[0] = value[1] = transport->volume;

	return 0;
}

static int bluealsa_write_integer(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, long *value) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;

	if (key / 2 > ctl->transports_count)
		return -EINVAL;

	struct msg_status status;
	struct msg_transport *transport = &ctl->transports[key / 2];
	struct request req = {
		.command = COMMAND_SET_TRANSPORT_VOLUME,
		.profile = transport->profile,
		.muted = transport->muted,
		.volume = transport->volume,
	};

	bacpy(&req.addr, &transport->addr);

	if (key % 2)
		req.muted = transport->muted = !*value;
	else
		req.volume = transport->volume = (value[0] + value[1]) / 2;

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
