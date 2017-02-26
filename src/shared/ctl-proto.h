/*
 * BlueALSA - ctl-proto.h
 * Copyright (c) 2016-2017 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_SHARED_CTLPROTO_H_
#define BLUEALSA_SHARED_CTLPROTO_H_

#if HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdint.h>
#include <bluetooth/bluetooth.h>

/* Location where the control socket and pipes are stored. */
#define BLUEALSA_RUN_STATE_DIR RUN_STATE_DIR "/bluealsa"

enum command {
	COMMAND_PING,
	COMMAND_LIST_DEVICES,
	COMMAND_LIST_TRANSPORTS,
	COMMAND_TRANSPORT_GET,
	COMMAND_TRANSPORT_SET_VOLUME,
	COMMAND_PCM_OPEN,
	COMMAND_PCM_CLOSE,
	COMMAND_PCM_PAUSE,
	COMMAND_PCM_RESUME,
	COMMAND_PCM_READY,
	__COMMAND_MAX
};

enum status_code {
	STATUS_CODE_SUCCESS = 0,
	STATUS_CODE_ERROR_UNKNOWN,
	STATUS_CODE_DEVICE_NOT_FOUND,
	STATUS_CODE_DEVICE_BUSY,
	STATUS_CODE_FORBIDDEN,
	STATUS_CODE_PONG,
};

enum pcm_type {
	PCM_TYPE_NULL = 0,
	PCM_TYPE_A2DP,
	PCM_TYPE_SCO,
};

enum pcm_stream {
	PCM_STREAM_PLAYBACK,
	PCM_STREAM_CAPTURE,
	/* Special stream type returned by the LIST_TRANSPORTS command to indicate,
	 * that given transport can act as a playback and capture device. In order
	 * to open PCM for such a transport, one has to provide one of PLAYBACK or
	 * CAPTURE stream types. */
	PCM_STREAM_DUPLEX,
};

struct __attribute__ ((packed)) request {

	enum command command;

	/* selected device address */
	bdaddr_t addr;

	/* requested transport type */
	enum pcm_type type;
	enum pcm_stream stream;

	/* fields used by the SET_TRANSPORT_VOLUME command */
	uint8_t ch1_muted:1;
	uint8_t ch1_volume:7;
	uint8_t ch2_muted:1;
	uint8_t ch2_volume:7;

};

/**
 * Single byte status message send by the controller at the end of every
 * response. This message contains the overall request status, which could
 * indicate either success or error. */
struct __attribute__ ((packed)) msg_status {
	uint8_t code;
};

struct __attribute__ ((packed)) msg_device {

	/* device address */
	bdaddr_t addr;
	/* name obtained from the Bluetooth device itself */
	char name[32];

	/* determine whether battery is available */
	uint8_t battery:1;
	/* device battery level in range [0, 9] */
	uint8_t battery_level:7;

};

struct __attribute__ ((packed)) msg_transport {

	/* device address for which the transport is created */
	bdaddr_t addr;

	/* selected profile and audio codec */
	enum pcm_type type;
	enum pcm_stream stream;
	uint8_t codec;

	/* number of audio channels */
	uint8_t channels;
	/* used sampling frequency */
	uint16_t sampling;

	/* Levels for channel 1 (left) and 2 (right). These fields are also
	 * used for SCO. In such a case channel 1 and 2 is responsible for
	 * respectively playback and capture. */
	uint8_t ch1_muted:1;
	uint8_t ch1_volume:7;
	uint8_t ch2_muted:1;
	uint8_t ch2_volume:7;

	/* transport delay in 1/10 of millisecond */
	uint16_t delay;

};

struct __attribute__ ((packed)) msg_pcm {
	struct msg_transport transport;
	char fifo[128];
};

#endif
