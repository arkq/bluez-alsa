/*
 * bluealsa-pcm.c
 * Copyright (c) 2016 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>

#include "ctl.h"
#include "transport.h"
#include "log.c"


/* Helper macro for obtaining the size of a static array. */
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))


struct bluealsa_pcm {
	snd_pcm_ioplug_t io;

	/* bluealsa socket */
	int fd;

	/* requested transport */
	struct msg_transport transport;
	size_t pcm_buffer_size;
	int pcm_fd;

	/* ALSA operates on frames, we on bytes */
	size_t frame_size;
	size_t buffer_size;

	/* fake ring buffer */
	size_t last_size;
	size_t pointer;

};


/**
 * Convert BlueALSA status message into the POSIX errno value. */
static int bluealsa_status_to_errno(struct msg_status *status) {
	switch (status->code) {
	case STATUS_CODE_SUCCESS:
		return 0;
	case STATUS_CODE_ERROR_UNKNOWN:
		return EIO;
	case STATUS_CODE_DEVICE_NOT_FOUND:
		return ENODEV;
	case STATUS_CODE_DEVICE_BUSY:
		return EBUSY;
	case STATUS_CODE_FORBIDDEN:
		return EACCES;
	default:
		/* some generic error code */
		return EINVAL;
	}
}

/**
 * Get PCM transport.
 *
 * @param pcm An address to the bluealsa pcm structure.
 * @return Upon success this function returns 0. Otherwise, -1 is returned. */
static int bluealsa_get_transport(struct bluealsa_pcm *pcm) {

	struct msg_status status = { 0xAB };
	struct request req = {
		.command = COMMAND_TRANSPORT_GET,
		.profile = pcm->transport.profile,
	};
	ssize_t len;

	bacpy(&req.addr, &pcm->transport.addr);

	debug("Getting transport for %s profile %d", batostr(&req.addr), req.profile);
	if (send(pcm->fd, &req, sizeof(req), MSG_NOSIGNAL) == -1)
		return -1;
	if ((len = read(pcm->fd, &pcm->transport, sizeof(pcm->transport))) == -1)
		return -1;

	/* in case of error, status message is returned */
	if (len != sizeof(pcm->transport)) {
		memcpy(&status, &pcm->transport, sizeof(status));
		errno = bluealsa_status_to_errno(&status);
		return -1;
	}

	if (read(pcm->fd, &status, sizeof(status)) == -1)
		return -1;

	return 0;
}

/**
 * Open PCM transport.
 *
 * @param pcm An address to the bluealsa pcm structure.
 * @return PCM FIFO file descriptor, or -1 on error. */
static int bluealsa_open_transport(struct bluealsa_pcm *pcm) {

	const int flags = pcm->io.stream == SND_PCM_STREAM_PLAYBACK ? O_WRONLY : O_RDONLY;
	struct msg_status status = { 0xAB };
	struct request req = {
		.command = COMMAND_PCM_OPEN,
		.profile = pcm->transport.profile,
	};
	struct msg_pcm res;
	ssize_t len;
	int fd;

	bacpy(&req.addr, &pcm->transport.addr);

	debug("Requesting PCM open for %s", batostr(&req.addr));
	if (send(pcm->fd, &req, sizeof(req), MSG_NOSIGNAL) == -1)
		return -1;
	if ((len = read(pcm->fd, &res, sizeof(res))) == -1)
		return -1;

	/* in case of error, status message is returned */
	if (len != sizeof(res)) {
		memcpy(&status, &res, sizeof(status));
		errno = bluealsa_status_to_errno(&status);
		return -1;
	}

	if (read(pcm->fd, &status, sizeof(status)) == -1)
		return -1;

	debug("Opening PCM FIFO (mode: %s): %s", flags == O_WRONLY ? "WR" : "RO", res.fifo);
	if ((fd = open(res.fifo, flags)) == -1)
		return -1;

	if (pcm->io.stream == SND_PCM_STREAM_PLAYBACK) {
		/* By default, the size of the pipe buffer is set to a too large value for
		 * our purpose. On modern Linux system it is 65536 bytes. Large buffer in
		 * the playback mode might contribute to an unnecessary audio delay. Since
		 * it is possible to modify the size of this buffer we will set is to some
		 * low value, but big enough to prevent audio tearing. Note, that the size
		 * will be rounded up to the page size (typically 4096 bytes). */
		pcm->pcm_buffer_size = fcntl(fd, F_SETPIPE_SZ, 2048);
		debug("FIFO buffer size: %zd", pcm->pcm_buffer_size);
	}

	return fd;
}

/**
 * Pause/resume PCM transport.
 *
 * @param pcm An address to the bluealsa pcm structure.
 * @param pause If non-zero, pause transport, otherwise resume it.
 * @return Upon success this function returns 0. Otherwise, -1 is returned. */
static int bluealsa_pause_transport(struct bluealsa_pcm *pcm, int pause) {

	struct msg_status status = { 0xAB };
	struct request req = {
		.command = pause ? COMMAND_PCM_PAUSE : COMMAND_PCM_RESUME,
		.profile = pcm->transport.profile,
	};

	bacpy(&req.addr, &pcm->transport.addr);

	debug("Requesting PCM %s for %s", pause ? "pause" : "resume", batostr(&req.addr));
	if (send(pcm->fd, &req, sizeof(req), MSG_NOSIGNAL) == -1)
		return -1;
	if (read(pcm->fd, &status, sizeof(status)) == -1)
		return -1;

	errno = bluealsa_status_to_errno(&status);
	return errno != 0 ? -1 : 0;
}

static int bluealsa_start(snd_pcm_ioplug_t *io) {
	(void)io;
	debug("Starting");
	return 0;
}

static int bluealsa_stop(snd_pcm_ioplug_t *io) {
	(void)io;
	debug("Stopping");
	return 0;
}

static snd_pcm_sframes_t bluealsa_pointer(snd_pcm_ioplug_t *io) {
	struct bluealsa_pcm *pcm = io->private_data;

	if (io->state == SND_PCM_STATE_XRUN)
		return -EPIPE;

	if (io->state != SND_PCM_STATE_RUNNING)
		return 0;

	size_t size = 0;

	if (io->stream == SND_PCM_STREAM_CAPTURE) {

		struct pollfd pfds[1] = {{ pcm->pcm_fd, POLLIN, 0 }};

		/* Wait until some data appears in the FIFO. It is required, because the
		 * IOCTL call (the next one) will not block, however we need some progress.
		 * Returning twice the same pointer will terminate reading. */
		if (poll(pfds, 1, -1) == -1)
			return -errno;

		if (ioctl(pcm->pcm_fd, FIONREAD, &size) == -1)
			return -errno;

		if (size > pcm->last_size) {
			pcm->pointer += size - pcm->last_size;
			pcm->pointer %= pcm->buffer_size;
		}

	}
	else {

		/* XXX: There is no FIONREAD for writing... */
		pcm->pointer++;
		pcm->pointer %= pcm->buffer_size;

	}

	pcm->last_size = size;
	return pcm->pointer / pcm->frame_size;
}

static int bluealsa_close(snd_pcm_ioplug_t *io) {
	struct bluealsa_pcm *pcm = io->private_data;
	debug("Closing plugin");
	close(pcm->fd);
	free(pcm);
	return 0;
}

static snd_pcm_sframes_t bluealsa_transfer_read(snd_pcm_ioplug_t *io,
		const snd_pcm_channel_area_t *areas, snd_pcm_uframes_t offset,
		snd_pcm_uframes_t size) {
	struct bluealsa_pcm *pcm = io->private_data;

	char *buffer = (char *)areas->addr + (areas->first + areas->step * offset) / 8;
	ssize_t len;

	if ((len = read(pcm->pcm_fd, buffer, size * pcm->frame_size)) == -1)
		return -errno;

	pcm->last_size -= len;
	return len / pcm->frame_size;
}

static snd_pcm_sframes_t bluealsa_transfer_write(snd_pcm_ioplug_t *io,
		const snd_pcm_channel_area_t *areas, snd_pcm_uframes_t offset,
		snd_pcm_uframes_t size) {
	struct bluealsa_pcm *pcm = io->private_data;

	const char *buffer = (char *)areas->addr + (areas->first + areas->step * offset) / 8;
	ssize_t len;

	if ((len = write(pcm->pcm_fd, buffer, size * pcm->frame_size)) == -1) {

		/* reading end has been closed */
		if (errno == EPIPE) {
			close(pcm->pcm_fd);
			pcm->pcm_fd = -1;
			return -ENODEV;
		}

		return -errno;
	}

	return len / pcm->frame_size;
}

static int bluealsa_hw_params(snd_pcm_ioplug_t *io, snd_pcm_hw_params_t *params) {
	struct bluealsa_pcm *pcm = io->private_data;
	(void)params;

	pcm->frame_size = (snd_pcm_format_physical_width(io->format) * io->channels) / 8;
	pcm->buffer_size = io->buffer_size * pcm->frame_size;

	if ((pcm->pcm_fd = bluealsa_open_transport(pcm)) == -1) {
		debug("Couldn't open PCM FIFO: %s", strerror(errno));
		return -errno;
	}

	io->poll_fd = pcm->pcm_fd;
	io->poll_events = io->stream == SND_PCM_STREAM_PLAYBACK ? POLLOUT : POLLIN;
	return snd_pcm_ioplug_reinit_status(io);
}

static int bluealsa_hw_free(snd_pcm_ioplug_t *io) {
	struct bluealsa_pcm *pcm = io->private_data;
	debug("Releasing PCM FIFO");
	if (pcm->pcm_fd != -1)
		close(pcm->pcm_fd);
	return 0;
}

static int bluealsa_prepare(snd_pcm_ioplug_t *io) {
	struct bluealsa_pcm *pcm = io->private_data;

	/* if PCM FIFO is not opened, report it right away */
	if (pcm->pcm_fd == -1)
		return -ENODEV;

	/* initialize "fake" ring buffer */
	pcm->last_size = 0;
	pcm->pointer = 0;

	debug("Prepared");
	return 0;
}

static int bluealsa_pause(snd_pcm_ioplug_t *io, int enable) {
	struct bluealsa_pcm *pcm = io->private_data;
	return bluealsa_pause_transport(pcm, enable) == -1 ? -errno : 0;
}

static void bluealsa_dump(snd_pcm_ioplug_t *io, snd_output_t *out) {
	struct bluealsa_pcm *pcm = io->private_data;
	char addr[18];

	ba2str(&pcm->transport.addr, addr);
	snd_output_printf(out, "Bluetooth device: %s\n", addr);
	snd_output_printf(out, "Bluetooth profile: %d\n", pcm->transport.profile);
	snd_output_printf(out, "Bluetooth codec: %d\n", pcm->transport.codec);
}

static int bluealsa_delay(snd_pcm_ioplug_t *io, snd_pcm_sframes_t *delayp) {
	struct bluealsa_pcm *pcm = io->private_data;

	/* Exact calculation of the PCM delay is very hard, if not impossible. For
	 * the sake of simplicity we will make few assumptions and approximations.
	 * In general, the delay is proportional to the number of bytes queued in
	 * the FIFO buffer, the time required to encode data, Bluetooth transfer
	 * latency and the time required by the device to decode and play audio. */

	snd_pcm_sframes_t delay = 0;

	if (io->stream == SND_PCM_STREAM_PLAYBACK)
		/* Since, it is not possible to obtain the number of bytes available for
		 * writing to the FIFO (there is no write related FIONREAD call), we will
		 * assume, that the whole buffer is filled (upper approximation). */
		delay += pcm->pcm_buffer_size / pcm->frame_size;
	else {
		size_t size;
		if (ioctl(pcm->pcm_fd, FIONREAD, &size) != -1)
			delay += size / pcm->frame_size;
	}

	/* TODO: Delay contribution from other components. */

	*delayp = delay;
	return 0;
}

static const snd_pcm_ioplug_callback_t bluealsa_a2dp_capture = {
	.start = bluealsa_start,
	.stop = bluealsa_stop,
	.pointer = bluealsa_pointer,
	.transfer = bluealsa_transfer_read,
	.close = bluealsa_close,
	.hw_params = bluealsa_hw_params,
	.hw_free = bluealsa_hw_free,
	.prepare = bluealsa_prepare,
	.dump = bluealsa_dump,
	.delay = bluealsa_delay,
};

static const snd_pcm_ioplug_callback_t bluealsa_a2dp_playback = {
	.start = bluealsa_start,
	.stop = bluealsa_stop,
	.pointer = bluealsa_pointer,
	.transfer = bluealsa_transfer_write,
	.close = bluealsa_close,
	.hw_params = bluealsa_hw_params,
	.hw_free = bluealsa_hw_free,
	.prepare = bluealsa_prepare,
	/* Drain callback is not required, because every data written to the PCM
	 * FIFO will be processed regardless of the ALSA client state - playback
	 * client can be closed immediately. */
	.drain = NULL,
	.pause = bluealsa_pause,
	.dump = bluealsa_dump,
	.delay = bluealsa_delay,
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

static int bluealsa_set_hw_constraint(struct bluealsa_pcm *pcm) {
	snd_pcm_ioplug_t *io = &pcm->io;

	static const snd_pcm_access_t accesses[] = {
		SND_PCM_ACCESS_RW_INTERLEAVED,
	};
	static const unsigned int formats[] = {
		SND_PCM_FORMAT_S16_LE,
	};

	int err;

	debug("Setting constraints");

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

	pcm->fd = -1;
	pcm->pcm_fd = -1;

	struct sockaddr_un saddr = { .sun_family = AF_UNIX };
	snprintf(saddr.sun_path, sizeof(saddr.sun_path) - 1,
			BLUEALSA_RUN_STATE_DIR "/%s", interface);

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
		ret = -errno;
		goto fail;
	}

	if (bluealsa_get_transport(pcm) == -1) {
		SNDERR("Cannot get BlueALSA transport: %s", strerror(errno));
		ret = -errno;
		goto fail;
	}

	pcm->io.version = SND_PCM_IOPLUG_VERSION;
	pcm->io.name = "BlueALSA";
	pcm->io.flags = SND_PCM_IOPLUG_FLAG_LISTED;
	pcm->io.callback = stream == SND_PCM_STREAM_CAPTURE ?
		&bluealsa_a2dp_capture : &bluealsa_a2dp_playback;
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
