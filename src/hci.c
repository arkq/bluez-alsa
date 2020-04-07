/*
 * BlueALSA - hci.c
 * Copyright (c) 2016-2019 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "hci.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/sco.h>

#include "shared/log.h"

/**
 * Get HCI local version (e.g. chip manufacturer).
 *
 * @param dev_id The ID of the HCI device.
 * @param ver Pointer to the HCI version structure for output data.
 * @return On success this function returns 0. Otherwise, -1 is returned
 *   and errno is set to indicate the error. */
int hci_get_version(int dev_id, struct hci_version *ver) {

	int dd;

	if ((dd = hci_open_dev(dev_id)) == -1)
		return -1;

	if (hci_read_local_version(dd, ver, 1000) == -1) {
		hci_close_dev(dd);
		return -1;
	}

	hci_close_dev(dd);
	return 0;
}

/**
 * Open SCO socket for given HCI device.
 *
 * @param dev_id The ID of the HCI device.
 * @return On success this function returns socket file descriptor. Otherwise,
 *   -1 is returned and errno is set to indicate the error. */
int hci_sco_open(int dev_id) {

	struct sockaddr_sco addr_hci = {
		.sco_family = AF_BLUETOOTH,
	};
	int dd, err;

	if (hci_devba(dev_id, &addr_hci.sco_bdaddr) == -1)
		return -1;
	if ((dd = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_SCO)) == -1)
		return -1;
	if (bind(dd, (struct sockaddr *)&addr_hci, sizeof(addr_hci)) == -1)
		goto fail;

	return dd;

fail:
	err = errno;
	close(dd);
	errno = err;
	return -1;
}

/**
 * Connect SCO socket with given BT device.
 *
 * @param sco_fd File descriptor of opened SCO socket.
 * @param ba Pointer to the Bluetooth address structure for a target device.
 * @param voice Bluetooth voice mode used during connection.
 * @return On success this function returns 0. Otherwise, -1 is returned and
 *   errno is set to indicate the error. */
int hci_sco_connect(int sco_fd, const bdaddr_t *ba, uint16_t voice) {

	struct sockaddr_sco addr_dev = {
		.sco_family = AF_BLUETOOTH,
		.sco_bdaddr = *ba,
	};

	struct bt_voice opt = { .setting = voice };
	if (setsockopt(sco_fd, SOL_BLUETOOTH, BT_VOICE, &opt, sizeof(opt)) == -1)
		return -1;

	struct timeval tv = { .tv_sec = 5 };
	if (setsockopt(sco_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == -1)
		warn("Couldn't set SCO connection timeout: %s", strerror(errno));

	if (connect(sco_fd, (struct sockaddr *)&addr_dev, sizeof(addr_dev)) == -1)
		return -1;

	return 0;
}

/**
 * Get read/write MTU for given SCO socket.
 *
 * @param sco_fd File descriptor of opened SCO socket.
 * @return On success this function returns MTU value. Otherwise, 0 is returned and
 *   errno is set to indicate the error. */
unsigned int hci_sco_get_mtu(int sco_fd) {

	struct sco_options options = { 0 };
	struct bt_voice voice = { 0 };
	socklen_t len;

	struct pollfd pfd = { sco_fd, POLLOUT, 0 };
	if (poll(&pfd, 1, -1) == -1)
		warn("Couldn't wait for SCO connection: %s", strerror(errno));

	len = sizeof(options);
	if (getsockopt(sco_fd, SOL_SCO, SCO_OPTIONS, &options, &len) == -1)
		warn("Couldn't get SCO socket options: %s", strerror(errno));

	len = sizeof(voice);
	if (getsockopt(sco_fd, SOL_BLUETOOTH, BT_VOICE, &voice, &len) == -1)
		warn("Couldn't get SCO voice options: %s", strerror(errno));

	debug("SCO link socket MTU: %d: %u", sco_fd, options.mtu);

	/* XXX: It seems, that the MTU value returned by kernel
	 *      is incorrect (or our interpretation of it). */

	options.mtu = 48;
	if (voice.setting == BT_VOICE_TRANSPARENT)
		options.mtu = 24;

	return options.mtu;
}

/**
 * Broadcom vendor HCI command for reading SCO routing configuration. */
int hci_bcm_read_sco_pcm_params(int dd, uint8_t *routing, uint8_t *clock,
		uint8_t *frame, uint8_t *sync, uint8_t *clk, int to) {

	struct __attribute__ ((packed)) {
		uint8_t status;
		uint8_t sco_routing;
		uint8_t pcm_interface_rate;
		uint8_t pcm_frame_type;
		uint8_t pcm_sync_mode;
		uint8_t pcm_clock_mode;
	} rp;

	struct hci_request rq = {
		.ogf = OGF_VENDOR_CMD,
		.ocf = 0x01D,
		.rparam = &rp,
		.rlen = sizeof(rp),
	};

	if (hci_send_req(dd, &rq, to) < 0)
		return -1;

	if (rp.status) {
		errno = EIO;
		return -1;
	}

	if (routing != NULL)
		*routing = rp.sco_routing;
	if (clock != NULL)
		*clock = rp.pcm_interface_rate;
	if (frame != NULL)
		*frame = rp.pcm_frame_type;
	if (sync != NULL)
		*sync = rp.pcm_sync_mode;
	if (clk != NULL)
		*clk = rp.pcm_clock_mode;

	return 0;
}

/**
 * Broadcom vendor HCI command for writing SCO routing configuration. */
int hci_bcm_write_sco_pcm_params(int dd, uint8_t routing, uint8_t clock,
		uint8_t frame, uint8_t sync, uint8_t clk, int to) {

	struct __attribute__ ((packed)) {
		uint8_t sco_routing;
		uint8_t pcm_interface_rate;
		uint8_t pcm_frame_type;
		uint8_t pcm_sync_mode;
		uint8_t pcm_clock_mode;
	} cp = { routing, clock, frame, sync, clk };
	uint8_t rp_status;

	struct hci_request rq = {
		.ogf = OGF_VENDOR_CMD,
		.ocf = 0x01C,
		.cparam = &cp,
		.clen = sizeof(cp),
		.rparam = &rp_status,
		.rlen = sizeof(rp_status),
	};

	if (hci_send_req(dd, &rq, to) < 0)
		return -1;

	if (rp_status) {
		errno = EIO;
		return -1;
	}

	return 0;
}

/**
 * Convert Bluetooth address into a human-readable string.
 *
 * This function returns statically allocated buffer. It is not by any means
 * thread safe. This function should be used for debugging purposes only.
 *
 * For debugging purposes, one could use the batostr() function provided by
 * the bluez library. However, this function converts the Bluetooth address
 * to the string with an incorrect (reversed) bytes order...
 *
 * @param ba Pointer to the Bluetooth address structure.
 * @return On success this function returns statically allocated buffer with
 *   human-readable Bluetooth address. On error, it returns NULL. */
const char *batostr_(const bdaddr_t *ba) {
	static char addr[18];
	if (ba2str(ba, addr) > 0)
		return addr;
	return NULL;
}
