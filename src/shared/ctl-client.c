/*
 * BlueALSA - ctl-client.c
 * Copyright (c) 2016-2018 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "shared/ctl-client.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>

#include "shared/log.h"


/**
 * Convert BlueALSA status message into the POSIX errno value. */
static int bluealsa_status_to_errno(const struct ba_msg_status *status) {
	switch (status->code) {
	case BA_STATUS_CODE_SUCCESS:
		return 0;
	case BA_STATUS_CODE_ERROR_UNKNOWN:
		return EIO;
	case BA_STATUS_CODE_DEVICE_NOT_FOUND:
		return ENODEV;
	case BA_STATUS_CODE_DEVICE_BUSY:
		return EBUSY;
	case BA_STATUS_CODE_FORBIDDEN:
		return EACCES;
	default:
		/* some generic error code */
		return EINVAL;
	}
}

#if DEBUG
/**
 * Convert Bluetooth address into a human-readable string.
 *
 * In order to convert Bluetooth address into the human-readable string, one
 * might use the ba2str() from the bluetooth library. However, it would be the
 * only used function from this library. In order to avoid excessive linking,
 * we are providing our own implementation of this function.
 *
 * @param ba Pointer to the Bluetooth address structure.
 * @param str Pointer to buffer big enough to contain string representation
 *   of the Bluetooth address.
 * @return Pointer to the destination string str. */
static char *ba2str_(const bdaddr_t *ba, char str[18]) {
	sprintf(str, "%2.2X:%2.2X:%2.2X:%2.2X:%2.2X:%2.2X",
			ba->b[5], ba->b[4], ba->b[3], ba->b[2], ba->b[1], ba->b[0]);
	return str;
}
#endif

/**
 * Send request to the BlueALSA server.
 *
 * @param fd Opened socket file descriptor.
 * @param req An address to the request structure.
 * @return Upon success this function returns 0. Otherwise, -1 is returned
 *   and errno is set appropriately. */
static int bluealsa_send_request(int fd, const struct ba_request *req) {

	struct ba_msg_status status = { 0xAB };

	if (send(fd, req, sizeof(*req), MSG_NOSIGNAL) == -1)
		return -1;
	if (read(fd, &status, sizeof(status)) == -1)
		return -1;

	errno = bluealsa_status_to_errno(&status);
	return errno != 0 ? -1 : 0;
}

/**
 * Open BlueALSA connection.
 *
 * @param interface HCI interface to use.
 * @return On success this function returns socket file descriptor. Otherwise,
 *   -1 is returned and errno is set to indicate the error. */
int bluealsa_open(const char *interface) {

	int fd, err;

	struct sockaddr_un saddr = { .sun_family = AF_UNIX };
	snprintf(saddr.sun_path, sizeof(saddr.sun_path) - 1,
			BLUEALSA_RUN_STATE_DIR "/%s", interface);

	if ((fd = socket(PF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0)) == -1)
		return -1;

	debug("Connecting to socket: %s", saddr.sun_path);
	if (connect(fd, (struct sockaddr *)(&saddr), sizeof(saddr)) == -1) {
		err = errno;
		close(fd);
		errno = err;
		return -1;
	}

	return fd;
}

/**
 * Subscribe for notifications.
 *
 * @param fd Opened socket file descriptor.
 * @param mask Bit-mask with events for which client wants to be subscribed.
 *   In order to cancel subscription, use empty event mask.
 * @return Upon success this function returns 0. Otherwise, -1 is returned
 *   and errno is set appropriately. */
int bluealsa_subscribe(int fd, enum ba_event mask) {
	const struct ba_request req = {
		.command = BA_COMMAND_SUBSCRIBE,
		.events = mask,
	};
	debug("Subscribing for events: %B", mask);
	return bluealsa_send_request(fd, &req);
}

/**
 * Get the list of connected Bluetooth devices.
 *
 * @param fd Opened socket file descriptor.
 * @param devices An address where the device list will be stored.
 * @return Upon success this function returns the number of connected devices
 *   and the `devices` address is modified to point to the devices list array,
 *   which should be freed with the free(). On error, -1 is returned and errno
 *   is set to indicate the error. */
ssize_t bluealsa_get_devices(int fd, struct ba_msg_device **devices) {

	const struct ba_request req = { .command = BA_COMMAND_LIST_DEVICES };
	struct ba_msg_device *_devices = NULL;
	struct ba_msg_device device;
	size_t i = 0;

	if (send(fd, &req, sizeof(req), MSG_NOSIGNAL) == -1)
		return -1;

	while (recv(fd, &device, sizeof(device), 0) == sizeof(device)) {
		_devices = realloc(_devices, (i + 1) * sizeof(*_devices));
		memcpy(&_devices[i], &device, sizeof(*_devices));
		i++;
	}

	*devices = _devices;
	return i;
}

/**
 * Get the list of available PCM transports.
 *
 * @param fd Opened socket file descriptor.
 * @param transports An address where the transport list will be stored.
 * @return Upon success this function returns the number of available PCM
 *   transports and the `transports` address is modified to point to the
 *   transport list array, which should be freed with the free(). On error,
 *   -1 is returned and errno is set to indicate the error. */
ssize_t bluealsa_get_transports(int fd, struct ba_msg_transport **transports) {

	const struct ba_request req = { .command = BA_COMMAND_LIST_TRANSPORTS };
	struct ba_msg_transport *_transports = NULL;
	struct ba_msg_transport transport;
	size_t i = 0;

	if (send(fd, &req, sizeof(req), MSG_NOSIGNAL) == -1)
		return -1;

	while (recv(fd, &transport, sizeof(transport), 0) == sizeof(transport)) {
		_transports = realloc(_transports, (i + 1) * sizeof(*_transports));
		memcpy(&_transports[i], &transport, sizeof(*_transports));
		i++;
	}

	*transports = _transports;
	return i;
}

/**
 * Get PCM transport.
 *
 * @param fd Opened socket file descriptor.
 * @param addr MAC address of the Bluetooth device.
 * @param type PCM type to get.
 * @param stream Stream direction to get, e.g. playback or capture.
 * @return Upon success this function returns pointer to the newly allocated
 *   transport structure, which should be freed with free(). Otherwise, NULL
 *   is returned and errno is set appropriately. */
struct ba_msg_transport *bluealsa_get_transport(int fd, bdaddr_t addr,
		enum ba_pcm_type type, enum ba_pcm_stream stream) {

	struct ba_msg_transport *transport;
	struct ba_msg_status status = { 0xAB };
	struct ba_request req = {
		.command = BA_COMMAND_TRANSPORT_GET,
		.addr = addr,
		.type = type,
		.stream = stream,
	};
	ssize_t len;

#if DEBUG
	char addr_[18];
	ba2str_(&req.addr, addr_);
	debug("Getting transport for %s type %d", addr_, type);
#endif

	if ((transport = malloc(sizeof(*transport))) == NULL)
		return NULL;

	if (send(fd, &req, sizeof(req), MSG_NOSIGNAL) == -1)
		return NULL;
	if ((len = read(fd, transport, sizeof(*transport))) == -1)
		return NULL;

	/* in case of error, status message is returned */
	if (len != sizeof(*transport)) {
		memcpy(&status, transport, sizeof(status));
		errno = bluealsa_status_to_errno(&status);
		return NULL;
	}

	if (read(fd, &status, sizeof(status)) == -1)
		return NULL;

	return transport;
}

/**
 * Get PCM transport delay.
 *
 * Note:
 * In fact, it is an alternative implementation of bluealsa_get_transport(),
 * which does not facilitates malloc().
 *
 * @param fd Opened socket file descriptor.
 * @param transport Address to the transport structure with the addr, type
 *   and stream fields set - other fields are not used by this function.
 * @return Upon success this function returns transport delay. Otherwise,
 *   -1 is returned and errno is set appropriately. */
int bluealsa_get_transport_delay(int fd, const struct ba_msg_transport *transport) {

	struct ba_msg_status status = { 0xAB };
	struct ba_msg_transport _transport;
	struct ba_request req = {
		.command = BA_COMMAND_TRANSPORT_GET,
		.addr = transport->addr,
		.type = transport->type,
		.stream = transport->stream,
	};
	ssize_t len;

	if (send(fd, &req, sizeof(req), MSG_NOSIGNAL) == -1)
		return -1;
	if ((len = read(fd, &_transport, sizeof(_transport))) == -1)
		return -1;

	/* in case of error, status message is returned */
	if (len != sizeof(_transport)) {
		memcpy(&status, &_transport, sizeof(status));
		errno = bluealsa_status_to_errno(&status);
		return -1;
	}

	if (read(fd, &status, sizeof(status)) == -1)
		return -1;

	return _transport.delay;
}

/**
 * Set PCM transport volume.
 *
 * @param fd Opened socket file descriptor.
 * @param transport Address to the transport structure with the addr, type
 *   and stream fields set - other fields are not used by this function.
 * @param ch1_muted It true, mute channel 1.
 * @param ch1_volume Channel 1 volume in range [0, 127].
 * @param ch2_muted If true, mute channel 2.
 * @param ch2_volume Channel 2 volume in range [0, 127].
 * @return Upon success this function returns 0. Otherwise, -1 is returned
 *   and errno is set appropriately. */
int bluealsa_set_transport_volume(int fd, const struct ba_msg_transport *transport,
		bool ch1_muted, int ch1_volume, bool ch2_muted, int ch2_volume) {

	struct ba_request req = {
		.command = BA_COMMAND_TRANSPORT_SET_VOLUME,
		.addr = transport->addr,
		.type = transport->type,
		.stream = transport->stream,
		.ch1_muted = ch1_muted,
		.ch1_volume = ch1_volume,
		.ch2_muted = ch2_muted,
		.ch2_volume = ch2_volume,
	};

	return bluealsa_send_request(fd, &req);
}

/**
 * Open PCM transport.
 *
 * @param fd Opened socket file descriptor.
 * @param transport Address to the transport structure with the addr, type
 *   and stream fields set - other fields are not used by this function.
 * @return PCM FIFO file descriptor, or -1 on error. */
int bluealsa_open_transport(int fd, const struct ba_msg_transport *transport) {

	struct ba_msg_status status = { 0xAB };
	struct ba_request req = {
		.command = BA_COMMAND_PCM_OPEN,
		.addr = transport->addr,
		.type = transport->type,
		.stream = transport->stream,
	};
	char buf[256] = "";
	struct iovec io = {
		.iov_base = &status,
		.iov_len = sizeof(status),
	};
	struct msghdr msg = {
		.msg_iov = &io,
		.msg_iovlen = 1,
		.msg_control = buf,
		.msg_controllen = sizeof(buf),
	};
	ssize_t len;

#if DEBUG
	char addr_[18];
	ba2str_(&req.addr, addr_);
	debug("Requesting PCM open for %s", addr_);
#endif

	if (send(fd, &req, sizeof(req), MSG_NOSIGNAL) == -1)
		return -1;
	if ((len = recvmsg(fd, &msg, MSG_CMSG_CLOEXEC)) == -1)
		return -1;

	struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
	if (cmsg == NULL ||
			cmsg->cmsg_level == IPPROTO_IP ||
			cmsg->cmsg_type == IP_TTL) {
		/* in case of error, status message is returned */
		errno = bluealsa_status_to_errno(&status);
		return -1;
	}

	if (read(fd, &status, sizeof(status)) == -1)
		return -1;

	return *((int *)CMSG_DATA(cmsg));
}

/**
 * Close PCM transport.
 *
 * @param fd Opened socket file descriptor.
 * @param transport Address to the transport structure with the addr, type
 *   and stream fields set - other fields are not used by this function.
 * @return Upon success this function returns 0. Otherwise, -1 is returned. */
int bluealsa_close_transport(int fd, const struct ba_msg_transport *transport) {

	struct ba_request req = {
		.command = BA_COMMAND_PCM_CLOSE,
		.addr = transport->addr,
		.type = transport->type,
		.stream = transport->stream,
	};

#if DEBUG
	char addr_[18];
	ba2str_(&req.addr, addr_);
	debug("Closing PCM for %s", addr_);
#endif

	return bluealsa_send_request(fd, &req);
}

/**
 * Pause/resume PCM transport.
 *
 * @param fd Opened socket file descriptor.
 * @param transport Address to the transport structure with the addr, type
 *   and stream fields set - other fields are not used by this function.
 * @param pause If non-zero, pause transport, otherwise resume it.
 * @return Upon success this function returns 0. Otherwise, -1 is returned. */
int bluealsa_pause_transport(int fd, const struct ba_msg_transport *transport, bool pause) {

	struct ba_request req = {
		.command = pause ? BA_COMMAND_PCM_PAUSE : BA_COMMAND_PCM_RESUME,
		.addr = transport->addr,
		.type = transport->type,
		.stream = transport->stream,
	};

#if DEBUG
	char addr_[18];
	ba2str_(&req.addr, addr_);
	debug("Requesting PCM %s for %s", pause ? "pause" : "resume", addr_);
#endif

	return bluealsa_send_request(fd, &req);
}

/**
 * Drain PCM transport.
 *
 * @param fd Opened socket file descriptor.
 * @param transport Address to the transport structure with the addr, type
 *   and stream fields set - other fields are not used by this function.
 * @return Upon success this function returns 0. Otherwise, -1 is returned. */
int bluealsa_drain_transport(int fd, const struct ba_msg_transport *transport) {

	struct ba_request req = {
		.command = BA_COMMAND_PCM_DRAIN,
		.addr = transport->addr,
		.type = transport->type,
		.stream = transport->stream,
	};

#if DEBUG
	char addr_[18];
	ba2str_(&req.addr, addr_);
	debug("Requesting PCM drain for %s", addr_);
#endif

	return bluealsa_send_request(fd, &req);
}

/**
 * Send RFCOMM message.
 *
 * @param fd Opened socket file descriptor.
 * @param addr MAC address of the Bluetooth device.
 * @param command NULL-terminated command string.
 * @return Upon success this function returns 0. Otherwise, -1 is returned. */
int bluealsa_send_rfcomm_command(int fd, bdaddr_t addr, const char *command) {

	struct ba_request req = {
		.command = BA_COMMAND_RFCOMM_SEND,
		.addr = addr,
	};

	/* snprintf() guarantees terminating NULL character */
	snprintf(req.rfcomm_command, sizeof(req.rfcomm_command), "%s", command);

#if DEBUG
	char addr_[18];
	ba2str_(&req.addr, addr_);
	debug("Sending RFCOMM command to %s: %s", addr_, req.rfcomm_command);
#endif

	return bluealsa_send_request(fd, &req);
}
