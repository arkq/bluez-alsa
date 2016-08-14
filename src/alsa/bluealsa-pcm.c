/*
 * bluealsa-pcm - bluealsa-pcm.c
 * Copyright (c) 2016 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <errno.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>

#include "ctl.h"
#include "log.c"
#include "transport.h"


/* Helper macro for obtaining the size of a static array. */
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))


struct bluealsa_pcm {
	snd_pcm_ioplug_t io;

	/* bluealsa socket */
	int fd;

	/* requested transport */
	struct msg_transport transport;
	int transport_fd;

	/* ALSA operates on frames, we on bytes */
	size_t frame_size;
	size_t buffer_size;

	/* fake ring buffer */
	size_t last_size;
	size_t pointer;

};


static int bluealsa_start(snd_pcm_ioplug_t *io) {
	struct bluealsa_pcm *pcm = io->private_data;

	ssize_t len;
	struct msg_pcm res;
	struct msg_status status = { 0xAB };
	struct request req = {
		.command = COMMAND_OPEN_PCM,
	};

	bacpy(&req.addr, &pcm->transport.addr);
	req.profile = pcm->transport.profile;

	send(pcm->fd, &req, sizeof(req), MSG_NOSIGNAL);
	if ((len = read(pcm->fd, &res, sizeof(res))) == -1)
		return -errno;

	if (len != sizeof(res)) {
		memcpy(&status, &res, sizeof(status));
		switch (status.code) {
		case STATUS_CODE_DEVICE_NOT_FOUND:
			return -ENODEV;
		case STATUS_CODE_DEVICE_BUSY:
			return -EBUSY;
		default:
			return -EFAULT;
		}
	}

	if ((len = read(pcm->fd, &status, sizeof(status))) == -1)
		return -errno;

	if ((pcm->transport_fd = open(res.fifo, O_RDONLY)) == -1)
		return -errno;

	/* prevent hijacking our precious data */
	unlink(res.fifo);

	debug("Started");
	return 0;
}

static int bluealsa_stop(snd_pcm_ioplug_t *io) {
	struct bluealsa_pcm *pcm = io->private_data;

	close(pcm->fd);
	if (pcm->transport_fd != -1)
		close(pcm->transport_fd);

	debug("Stopped");
	return 0;
}

static snd_pcm_sframes_t bluealsa_pointer(snd_pcm_ioplug_t *io) {
	struct bluealsa_pcm *pcm = io->private_data;

	if (io->state == SND_PCM_STATE_XRUN)
		return -EPIPE;

	if (io->state != SND_PCM_STATE_RUNNING)
		return 0;

	size_t size = 0;
	struct pollfd pfds[1] = {{ pcm->transport_fd, POLLIN, 0 }};

	/* Wait until some data appears in the FIFO. It is required, because the
	 * IOCTL call (the next one) will not block, however we need some progress.
	 * Returning twice the same pointer will terminate reading. */
	if (poll(pfds, 1, -1) == -1)
		return -errno;

	if (ioctl(pcm->transport_fd, FIONREAD, &size) == -1)
		return -errno;

	if (size > pcm->last_size) {
		pcm->pointer += size - pcm->last_size;
		pcm->pointer %= pcm->buffer_size;
	}

	pcm->last_size = size;

	return snd_pcm_bytes_to_frames(io->pcm, pcm->pointer);
}

static snd_pcm_sframes_t bluealsa_transfer_read(snd_pcm_ioplug_t *io,
		const snd_pcm_channel_area_t *areas, snd_pcm_uframes_t offset,
		snd_pcm_uframes_t size) {
	struct bluealsa_pcm *pcm = io->private_data;

	char *buffer = (char *)areas->addr + (areas->first + areas->step * offset) / 8;
	ssize_t len;

	if ((len = read(pcm->transport_fd, buffer, size * pcm->frame_size)) <= 0)
		return len;

	pcm->last_size -= len;
	return len / pcm->frame_size;
}

static snd_pcm_sframes_t bluealsa_transfer_write(snd_pcm_ioplug_t *io,
		const snd_pcm_channel_area_t *areas, snd_pcm_uframes_t offset,
		snd_pcm_uframes_t size) {
	struct bluealsa_pcm *pcm = io->private_data;

	SNDERR("write");
	return 0;
}

static int bluealsa_close(snd_pcm_ioplug_t *io) {
	free(io->private_data);
	debug("Closed");
	return 0;
}

static int bluealsa_hw_params(snd_pcm_ioplug_t *io, snd_pcm_hw_params_t *params) {
	struct bluealsa_pcm *pcm = io->private_data;
	(void)params;

	pcm->frame_size = (snd_pcm_format_physical_width(io->format) * io->channels) / 8;
	pcm->buffer_size = io->buffer_size * pcm->frame_size;

	debug("HW params obtained");
	return 0;
}

static int bluealsa_prepare(snd_pcm_ioplug_t *io) {
	struct bluealsa_pcm *pcm = io->private_data;

	/* initialize "fake" ring buffer */
	pcm->last_size = 0;
	pcm->pointer = 0;

	debug("Prepared");
	return 0;
}

static int bluealsa_drain(snd_pcm_ioplug_t *io) {
	struct bluealsa_pcm *pcm = io->private_data;
	static char buffer[512];

	while (read(pcm->transport_fd, buffer, sizeof(buffer)) > 0)
		continue;

	debug("Drained");
	return 0;
}

static const snd_pcm_ioplug_callback_t bluealsa_a2dp_playback = {
	.start = bluealsa_start,
	.stop = bluealsa_stop,
	.pointer = bluealsa_pointer,
	.transfer = bluealsa_transfer_write,
	.close = bluealsa_close,
	.hw_params = bluealsa_hw_params,
	.prepare = bluealsa_prepare,
};

static const snd_pcm_ioplug_callback_t bluealsa_a2dp_capture = {
	.start = bluealsa_start,
	.stop = bluealsa_stop,
	.pointer = bluealsa_pointer,
	.transfer = bluealsa_transfer_read,
	.close = bluealsa_close,
	.hw_params = bluealsa_hw_params,
	.prepare = bluealsa_prepare,
	.drain = bluealsa_drain,
};

uint8_t bluealsa_parse_profile(const char *profile, snd_pcm_stream_t stream) {

	if (profile == NULL)
		return 0;

	if (strcasecmp(profile, "a2dp") == 0) {
		return stream == SND_PCM_STREAM_PLAYBACK ?
			TRANSPORT_PROFILE_A2DP_SOURCE : TRANSPORT_PROFILE_A2DP_SINK;
	}

	return 0;
}

static int bluealsa_get_transport(struct bluealsa_pcm *pcm) {

	ssize_t len;
	struct msg_status status = { 0xAB };
	struct request req = {
		.command = COMMAND_GET_TRANSPORT,
	};

	bacpy(&req.addr, &pcm->transport.addr);
	req.profile = pcm->transport.profile;

	send(pcm->fd, &req, sizeof(req), MSG_NOSIGNAL);

	if ((len = read(pcm->fd, &pcm->transport, sizeof(pcm->transport))) == -1)
		return -errno;

	if (len != sizeof(pcm->transport)) {
		memcpy(&status, &pcm->transport, sizeof(status));
		return status.code == STATUS_CODE_DEVICE_NOT_FOUND ? -ENODEV : -EFAULT;
	}

	if ((len = read(pcm->fd, &status, sizeof(status))) == -1)
		return -errno;

	return 0;
}

static int bluealsa_set_hw_constraint(struct bluealsa_pcm *pcm) {
	snd_pcm_ioplug_t *io = &pcm->io;

	static const snd_pcm_access_t accesses[] = {
		SND_PCM_ACCESS_RW_INTERLEAVED,
	};
	static const unsigned int formats[] = {
		SND_PCM_FORMAT_S16_LE,
	};

	int err;

	if ((err = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_ACCESS,
					ARRAY_SIZE(accesses), accesses)) < 0)
		return err;

	if ((err = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_FORMAT,
					ARRAY_SIZE(formats), formats)) < 0)
		return err;

	if ((err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_BUFFER_BYTES,
					8192 * 3, 8192 * 3)) < 0)
		return err;

	if ((err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_CHANNELS,
					pcm->transport.channels, pcm->transport.channels)) < 0)
		return err;

	if ((err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_RATE,
					pcm->transport.sampling, pcm->transport.sampling)) < 0)
		return err;

	return 0;
}

SND_PCM_PLUGIN_DEFINE_FUNC(bluealsa) {
	(void)root;

	snd_config_iterator_t i, next;
	const char *interface = "hci0";
	const char *device = NULL;
	const char *profile = NULL;
	struct bluealsa_pcm *pcm;
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
		if (strcmp(id, "device") == 0) {
			if (snd_config_get_string(n, &device) < 0) {
				SNDERR("Invalid type for %s", id);
				return -EINVAL;
			}
			continue;
		}
		if (strcmp(id, "profile") == 0) {
			if (snd_config_get_string(n, &profile) < 0) {
				SNDERR("Invalid type for %s", id);
				return -EINVAL;
			}
			continue;
		}

		SNDERR("Unknown field %s", id);
		return -EINVAL;
	}

	if ((pcm = calloc(1, sizeof(*pcm))) == NULL)
		return -ENOMEM;

	struct sockaddr_un saddr = { .sun_family = AF_UNIX };
	snprintf(saddr.sun_path, sizeof(saddr.sun_path) - 1,
			BLUEALSA_RUN_STATE_DIR "/%s", interface);
	pcm->fd = -1;
	pcm->transport_fd = -1;

	if (device == NULL || str2ba(device, &pcm->transport.addr) != 0) {
		SNDERR("Invalid BT device address: %s", device);
		ret = -EINVAL;
		goto fail;
	}

	if ((pcm->transport.profile = bluealsa_parse_profile(profile, stream)) == 0) {
		SNDERR("Invalid BT profile: %s", profile);
		ret = -EINVAL;
		goto fail;
	}

	if ((pcm->fd = socket(PF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) == -1) {
		ret = -errno;
		goto fail;
	}

	if (connect(pcm->fd, (struct sockaddr *)(&saddr), sizeof(saddr)) == -1) {
		SNDERR("BlueALSA connection failed: %s", strerror(errno));
		ret = -ENODEV;
		goto fail;
	}

	if ((ret = bluealsa_get_transport(pcm)) < 0) {
		SNDERR("Cannot get BlueALSA transport: %s", strerror(-ret));
		goto fail;
	}

	pcm->io.version = SND_PCM_EXTPLUG_VERSION;
	pcm->io.name = "BlueALSA";
	pcm->io.flags = SND_PCM_IOPLUG_FLAG_LISTED;
	pcm->io.callback = stream == SND_PCM_STREAM_PLAYBACK ?
		&bluealsa_a2dp_playback : &bluealsa_a2dp_capture;
	pcm->io.private_data = pcm;

	if ((ret = snd_pcm_ioplug_create(&pcm->io, name, stream, mode)) < 0)
		goto fail;

	if ((ret = bluealsa_set_hw_constraint(pcm)) < 0) {
		snd_pcm_ioplug_delete(&pcm->io);
		goto fail;
	}

	*pcmp = pcm->io.pcm;
	return 0;

fail:
	if (pcm->fd != -1)
		close(pcm->fd);
	free(pcm);
	return ret;
}

SND_PCM_PLUGIN_SYMBOL(bluealsa);
