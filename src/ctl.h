/*
 * bluealsa - ctl.h
 * Copyright (c) 2016 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_CTL_H_
#define BLUEALSA_CTL_H_

#if HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdint.h>
#include <bluetooth/bluetooth.h>

/* Location where the control socket and pipes are stored. */
#define BLUEALSA_RUN_STATE_DIR RUN_STATE_DIR "/bluealsa"

/* Maximal number of clients connected to the controller. */
#define BLUEALSA_MAX_CLIENTS 7

enum command {
	COMMAND_PING,
	COMMAND_LIST_DEVICES,
	COMMAND_LIST_TRANSPORTS,
	COMMAND_GET_TRANSPORT,
	COMMAND_OPEN_PCM,
	__COMMAND_MAX
};

enum status_code {
	STATUS_CODE_SUCCESS = 0,
	STATUS_CODE_ERROR_UNKNOWN,
	STATUS_CODE_DEVICE_NOT_FOUND,
	STATUS_CODE_PONG,
};

struct __attribute__ ((packed)) request {

	enum command command;

	/* selected device address */
	bdaddr_t addr;

	/* requested transport type */
	uint8_t profile;
	uint8_t codec;

};

/**
 * Single byte status message send by the controller at the end of every
 * response. This message contains the overall request status, which could
 * indicate either success or error. */
struct __attribute__ ((packed)) msg_status {
	uint8_t code;
};

struct __attribute__ ((packed)) msg_device {
	bdaddr_t addr;
	char name[32];
};

struct __attribute__ ((packed)) msg_transport {

	/* device address for which the transport is created */
	bdaddr_t addr;

	/* transport name - most likely generic profile name */
	char name[32];

	/* selected profile and audio codec */
	uint8_t profile;
	/* TODO: Is codec required?? It's more like internal stuff. */
	uint8_t codec;

	/* number of audio channels */
	uint8_t channels;
	/* used sampling frequency */
	uint16_t sampling;

	uint8_t muted:1;
	uint8_t volume:7;

};

struct __attribute__ ((packed)) msg_pcm {
	struct msg_transport transport;
	char fifo[128];
};

/* XXX: These functions are not exported in any library. */
int ctl_thread_init(const char *device, void *userdata);
void ctl_free();

#endif
