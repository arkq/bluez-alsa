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
# include "../config.h"
#endif

#include <stdint.h>
#include <bluetooth/bluetooth.h>

/* Location where control socket and pipes are stored. */
#define BLUEALSA_RUN_STATE_DIR RUN_STATE_DIR "/bluealsa"

/* Maximal number of clients connected to the controller. */
#define BLUEALSA_CTL_MAX_CLIENTS 7

enum ctl_command {
	CTL_COMMAND_PING,
	CTL_COMMAND_LIST_DEVICES,
	CTL_COMMAND_LIST_TRANSPORTS,
	CTL_COMMAND_GET_TRANSPORT,
	CTL_COMMAND_OPEN_PCM,
	__CTL_COMMAND_MAX
};

/* List of supported Bluetooth profiles. */
enum ctl_transport_type {
	CTL_TRANSPORT_TYPE_DISABLED = 0,
	CTL_TRANSPORT_TYPE_A2DP_SOURCE,
	CTL_TRANSPORT_TYPE_A2DP_SINK,
	CTL_TRANSPORT_TYPE_HFP,
	CTL_TRANSPORT_TYPE_HSP,
	__CTL_TRANSPORT_TYPE_MAX
};

/* List of supported sampling frequencies. */
enum ctl_transport_sp_freq {
	CTL_TRANSPORT_SP_FREQ_16000,
	CTL_TRANSPORT_SP_FREQ_32000,
	CTL_TRANSPORT_SP_FREQ_44100,
	CTL_TRANSPORT_SP_FREQ_48000,
	__CTL_TRANSPORT_SP_FREQ_MAX
};

struct __attribute__ ((packed)) ctl_request {

	enum ctl_command command;

	/* fields used for selecting transport */
	bdaddr_t addr;
	uint8_t type;

	union {
		uint8_t volume;
	};

};

/* Single byte string send by the controller to indicate end of
 * data for list responses or "not-found" for get requests. */
#define CTL_END "\xED"

struct __attribute__ ((packed)) ctl_device {
	bdaddr_t addr;
	char name[32];
};

struct __attribute__ ((packed)) ctl_transport {

	/* device address for which the transport is created with
	 * the transport type create unique transport identifier */
	bdaddr_t addr;
	uint8_t type;

	uint8_t _pad1;
	char name[32];

	/* number of audio channels */
	uint8_t channels:2;
	/* used sampling frequency */
	uint8_t frequency:2;

	uint8_t volume;

};

struct __attribute__ ((packed)) ctl_pcm {
	struct ctl_transport transport;
	char fifo[128];
};

/* XXX: These functions are not exported in any library. */
int ctl_thread_init(const char *device, void *userdata);
void ctl_free();

#endif
