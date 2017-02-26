/*
 * BlueALSA - ctl-client.c
 * Copyright (c) 2016-2017 Arkadiusz Bokowy
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
static int bluealsa_status_to_errno(const struct msg_status *status) {
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
static int bluealsa_send_request(int fd, const struct request *req) {

	struct msg_status status = { 0xAB };

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

	if ((fd = socket(PF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) == -1)
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
 * Get the list of connected Bluetooth devices.
 *
 * @param fd Opened socket file descriptor.
 * @param devices An address where the device list will be stored.
 * @return Upon success this function returns the number of connected devices
 *   and the `devices` address is modified to point to the devices list array,
 *   which should be freed with the free(). On error, -1 is returned and errno
 *   is set to indicate the error. */
ssize_t bluealsa_get_devices(int fd, struct msg_device **devices) {

	const struct request req = { .command = COMMAND_LIST_DEVICES };
	struct msg_device *_devices = NULL;
	struct msg_device device;
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
ssize_t bluealsa_get_transports(int fd, struct msg_transport **transports) {

	const struct request req = { .command = COMMAND_LIST_TRANSPORTS };
	struct msg_transport *_transports = NULL;
	struct msg_transport transport;
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
struct msg_transport *bluealsa_get_transport(int fd, bdaddr_t addr,
		enum pcm_type type, enum pcm_stream stream) {

	struct msg_transport *transport;
	struct msg_status status = { 0xAB };
	struct request req = {
		.command = COMMAND_TRANSPORT_GET,
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
int bluealsa_get_transport_delay(int fd, const struct msg_transport *transport) {

	struct msg_status status = { 0xAB };
	struct msg_transport _transport;
	struct request req = {
		.command = COMMAND_TRANSPORT_GET,
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
 * Open PCM transport.
 *
 * @param fd Opened socket file descriptor.
 * @param transport Address to the transport structure with the addr, type
 *   and stream fields set - other fields are not used by this function.
 * @return PCM FIFO file descriptor, or -1 on error. */
int bluealsa_open_transport(int fd, const struct msg_transport *transport) {

	struct msg_status status = { 0xAB };
	struct request req = {
		.command = COMMAND_PCM_OPEN,
		.addr = transport->addr,
		.type = transport->type,
		.stream = transport->stream,
	};
	struct msg_pcm res;
	ssize_t len;
	int pcm;

#if DEBUG
	char addr_[18];
	ba2str_(&req.addr, addr_);
	debug("Requesting PCM open for %s", addr_);
#endif

	if (send(fd, &req, sizeof(req), MSG_NOSIGNAL) == -1)
		return -1;
	if ((len = read(fd, &res, sizeof(res))) == -1)
		return -1;

	/* in case of error, status message is returned */
	if (len != sizeof(res)) {
		memcpy(&status, &res, sizeof(status));
		errno = bluealsa_status_to_errno(&status);
		return -1;
	}

	if (read(fd, &status, sizeof(status)) == -1)
		return -1;

	debug("Opening PCM FIFO (mode: %s): %s",
			req.stream == PCM_STREAM_PLAYBACK ? "WR" : "RO", res.fifo);
	if ((pcm = open(res.fifo, req.stream == PCM_STREAM_PLAYBACK ?
					O_WRONLY : O_RDONLY | O_NONBLOCK)) == -1)
		return -1;

	/* Restore the blocking mode. Non-blocking mode was required only for the
	 * opening stage - FIFO read-write sides synchronization is done in the IO
	 * thread. */
	if (req.stream == PCM_STREAM_CAPTURE)
		fcntl(pcm, F_SETFL, fcntl(pcm, F_GETFL) & ~O_NONBLOCK);

	/* In the capture mode it is required to signal the server, that the PCM
	 * opening process has been finished. This requirement comes from the fact,
	 * that the writing side of the FIFO will not be opened before the reading
	 * side is (if the write-only non-blocking mode is used). This "PCM ready"
	 * signal will help to synchronize FIFO opening process. */
	if (req.stream == PCM_STREAM_CAPTURE) {
		req.command = COMMAND_PCM_READY;
		bluealsa_send_request(fd, &req);
	}

	return pcm;
}

/**
 * Close PCM transport.
 *
 * @param fd Opened socket file descriptor.
 * @param transport Address to the transport structure with the addr, type
 *   and stream fields set - other fields are not used by this function.
 * @return Upon success this function returns 0. Otherwise, -1 is returned. */
int bluealsa_close_transport(int fd, const struct msg_transport *transport) {

	struct request req = {
		.command = COMMAND_PCM_CLOSE,
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
int bluealsa_pause_transport(int fd, const struct msg_transport *transport, bool pause) {

	struct request req = {
		.command = pause ? COMMAND_PCM_PAUSE : COMMAND_PCM_RESUME,
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
