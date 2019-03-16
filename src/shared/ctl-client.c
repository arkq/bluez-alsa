/*
 * BlueALSA - ctl-client.c
 * Copyright (c) 2016-2019 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "shared/ctl-client.h"

#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

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
	case BA_STATUS_CODE_STREAM_NOT_FOUND:
		return ENXIO;
	case BA_STATUS_CODE_CODEC_NOT_SELECTED:
		return ENOENT;
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

	const uint16_t ver = BLUEALSA_CRL_PROTO_VERSION;
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

	if (send(fd, &ver, sizeof(ver), MSG_NOSIGNAL) == -1)
		return -1;

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
int bluealsa_event_subscribe(int fd, uint8_t mask) {
	const struct ba_request req = {
		.command = BA_COMMAND_SUBSCRIBE,
		.events = mask,
	};
	debug("Subscribing for events: %B", mask);
	return bluealsa_send_request(fd, &req);
}

/**
 * Check whether event matches given transport.
 *
 * @param transport Address to the transport structure with the addr and
 *   type fields set - other fields are not used by this function.
 * @parem event Address to the event structure.
 * @return This function returns 0 when event matches given transport,
 *   and any other value otherwise. */
int bluealsa_event_match(const struct ba_msg_transport *transport,
		const struct ba_msg_event *event) {
	int ret;
	if ((ret = bacmp(&transport->addr, &event->addr)) != 0)
		return ret;
	if ((ret = BA_PCM_TYPE(transport->type) - BA_PCM_TYPE(event->type)) != 0)
		return ret;
	return transport->type - (transport->type & event->type);
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
 * @param addr Pointer to the Bluetooth address structure.
 * @param type PCM type (with stream direction) to get.
 * @param transport An address where the transport will be stored.
 * @return Upon success this function returns 0. Otherwise, -1 is returned
 *   and errno is set appropriately. */
int bluealsa_get_transport(int fd, const bdaddr_t *addr, uint8_t type,
		struct ba_msg_transport *transport) {

	struct ba_msg_status status = { 0xAB };
	struct ba_request req = {
		.command = BA_COMMAND_TRANSPORT_GET,
		.addr = *addr,
		.type = type,
	};
	ssize_t len;

#if DEBUG
	char addr_[18];
	ba2str_(&req.addr, addr_);
	debug("Getting transport for %s type %#x", addr_, type);
#endif

	if (send(fd, &req, sizeof(req), MSG_NOSIGNAL) == -1)
		return -1;
	if ((len = read(fd, transport, sizeof(*transport))) == -1)
		return -1;

	/* in case of error, status message is returned */
	if (len != sizeof(*transport)) {
		memcpy(&status, transport, sizeof(status));
		errno = bluealsa_status_to_errno(&status);
		return -1;
	}

	if (read(fd, &status, sizeof(status)) == -1)
		return -1;

	/* For SCO transport, server will report that both streaming directions are
	 * possible. Then, when one will use such a transport structure for opening
	 * PCM, it will be ambiguous which PCM to open. In order to prevent such a
	 * scenario, update transport type with the requested one. */
	transport->type = type;

	return 0;
}

/**
 * Get PCM transport delay.
 *
 * @param fd Opened socket file descriptor.
 * @param transport Address to the transport structure with the addr and
 *   type fields set - other fields are not used by this function.
 * @param delay An address where the transport delay will be stored.
 * @return Upon success this function returns 0. Otherwise, -1 is returned
 *   and errno is set appropriately. */
int bluealsa_get_transport_delay(int fd, const struct ba_msg_transport *transport,
		unsigned int *delay) {

	struct ba_msg_transport t;
	int ret;

	if ((ret = bluealsa_get_transport(fd, &transport->addr,
					transport->type, &t)) == 0)
		*delay = t.delay;

	return ret;
}

/**
 * Set PCM transport delay.
 *
 * @param fd Opened socket file descriptor.
 * @param transport Address to the transport structure with the addr and
 *   type fields set - other fields are not used by this function.
 * @param delay Transport delay.
 * @return Upon success this function returns 0. Otherwise, -1 is returned
 *   and errno is set appropriately. */
int bluealsa_set_transport_delay(int fd, const struct ba_msg_transport *transport,
		unsigned int delay) {

	struct ba_request req = {
		.command = BA_COMMAND_TRANSPORT_SET_DELAY,
		.addr = transport->addr,
		.type = transport->type,
		.delay = delay,
	};

	return bluealsa_send_request(fd, &req);
}

/**
 * Get PCM transport volume.
 *
 * @param fd Opened socket file descriptor.
 * @param transport Address to the transport structure with the addr and
 *   type fields set - other fields are not used by this function.
 * @param ch1_muted An address where the mute of channel 1 will be stored.
 * @param ch1_volume An address where the volume of channel 1 will be stored.
 * @param ch2_muted An address where the mute of channel 2 will be stored.
 * @param ch2_volume An address where the volume of channel 2 will be stored.
 * @return Upon success this function returns 0. Otherwise, -1 is returned
 *   and errno is set appropriately. */
int bluealsa_get_transport_volume(int fd, const struct ba_msg_transport *transport,
		bool *ch1_muted, int *ch1_volume, bool *ch2_muted, int *ch2_volume) {

	struct ba_msg_transport t;
	int ret;

	if ((ret = bluealsa_get_transport(fd, &transport->addr,
					transport->type, &t)) == 0) {
		*ch1_muted = t.ch1_muted;
		*ch1_volume = t.ch1_volume;
		*ch2_muted = t.ch2_muted;
		*ch2_volume = t.ch2_volume;
	}

	return ret;
}

/**
 * Set PCM transport volume.
 *
 * @param fd Opened socket file descriptor.
 * @param transport Address to the transport structure with the addr and
 *   type fields set - other fields are not used by this function.
 * @param ch1_muted If true, mute channel 1.
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
 * @param transport Address to the transport structure with the addr and
 *   type fields set - other fields are not used by this function.
 * @return PCM FIFO file descriptor, or -1 on error. */
int bluealsa_open_transport(int fd, const struct ba_msg_transport *transport) {

	struct ba_msg_status status = { 0xAB };
	struct ba_request req = {
		.command = BA_COMMAND_PCM_OPEN,
		.addr = transport->addr,
		.type = transport->type,
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
 * Control opened PCM transport.
 *
 * @param fd Opened socket file descriptor.
 * @param transport Address to the transport structure with the addr and
 *   type fields set - other fields are not used by this function.
 * @param cmd PCM control command, one of: PAUSE, RESUME, DRAIN, DROP.
 * @return Upon success this function returns 0. Otherwise, -1 is returned. */
int bluealsa_control_transport(int fd, const struct ba_msg_transport *transport, enum ba_command cmd) {

	struct ba_request req = {
		.command = cmd,
		.addr = transport->addr,
		.type = transport->type,
	};

#if DEBUG
	static const char *desc[] = {
		[BA_COMMAND_PCM_PAUSE] = "pause",
		[BA_COMMAND_PCM_RESUME] = "resume",
		[BA_COMMAND_PCM_DRAIN] = "drain",
		[BA_COMMAND_PCM_DROP] = "drop" };
	char addr_[18];
	ba2str_(&req.addr, addr_);
	debug("Requesting PCM %s for %s", desc[cmd], addr_);
#endif

	return bluealsa_send_request(fd, &req);
}

/**
 * Send RFCOMM message.
 *
 * @param fd Opened socket file descriptor.
 * @param addr Pointer to the Bluetooth address structure.
 * @param command NULL-terminated command string.
 * @return Upon success this function returns 0. Otherwise, -1 is returned. */
int bluealsa_send_rfcomm_command(int fd, const bdaddr_t *addr, const char *command) {

	struct ba_request req = {
		.command = BA_COMMAND_RFCOMM_SEND,
		.addr = *addr,
	};

	strncpy(req.rfcomm_command, command, sizeof(req.rfcomm_command));
	req.rfcomm_command[sizeof(req.rfcomm_command) - 1] = '\0';

#if DEBUG
	char addr_[18];
	ba2str_(&req.addr, addr_);
	debug("Sending RFCOMM command to %s: %s", addr_, req.rfcomm_command);
#endif

	return bluealsa_send_request(fd, &req);
}
