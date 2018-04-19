/*
 * BlueALSA - ctl-proto.h
 * Copyright (c) 2016-2018 Arkadiusz Bokowy
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

enum ba_command {
	BA_COMMAND_PING,
	BA_COMMAND_SUBSCRIBE,
	BA_COMMAND_LIST_DEVICES,
	BA_COMMAND_LIST_TRANSPORTS,
	BA_COMMAND_TRANSPORT_GET,
	BA_COMMAND_TRANSPORT_SET_VOLUME,
	BA_COMMAND_PCM_OPEN,
	BA_COMMAND_PCM_CLOSE,
	BA_COMMAND_PCM_PAUSE,
	BA_COMMAND_PCM_RESUME,
	BA_COMMAND_PCM_DRAIN,
	BA_COMMAND_RFCOMM_SEND,
	__BA_COMMAND_MAX
};

enum ba_status_code {
	BA_STATUS_CODE_SUCCESS = 0,
	BA_STATUS_CODE_ERROR_UNKNOWN,
	BA_STATUS_CODE_DEVICE_NOT_FOUND,
	BA_STATUS_CODE_DEVICE_BUSY,
	BA_STATUS_CODE_FORBIDDEN,
	BA_STATUS_CODE_PONG,
};

enum ba_event {
	BA_EVENT_TRANSPORT_ADDED   = 1 << 0,
	BA_EVENT_TRANSPORT_CHANGED = 1 << 1,
	BA_EVENT_TRANSPORT_REMOVED = 1 << 2,
	BA_EVENT_UPDATE_BATTERY    = 1 << 3,
	BA_EVENT_UPDATE_VOLUME     = 1 << 4,
};

enum ba_pcm_type {
	BA_PCM_TYPE_NULL = 0,
	BA_PCM_TYPE_A2DP,
	BA_PCM_TYPE_SCO,
};

enum ba_pcm_stream {
	BA_PCM_STREAM_PLAYBACK,
	BA_PCM_STREAM_CAPTURE,
	/* Special stream type returned by the LIST_TRANSPORTS command to indicate,
	 * that given transport can act as a playback and capture device. In order
	 * to open PCM for such a transport, one has to provide one of PLAYBACK or
	 * CAPTURE stream types. */
	BA_PCM_STREAM_DUPLEX,
};

struct __attribute__ ((packed)) ba_request {

	enum ba_command command;

	/* selected device address */
	bdaddr_t addr;

	/* requested transport type */
	enum ba_pcm_type type;
	enum ba_pcm_stream stream;

	/* bit-mask with event subscriptions */
	enum ba_event events;

	/* fields used by the TRANSPORT_SET_VOLUME command */
	uint8_t ch1_muted:1;
	uint8_t ch1_volume:7;
	uint8_t ch2_muted:1;
	uint8_t ch2_volume:7;

	/* RFCOMM command string to send */
	char rfcomm_command[32];

};

/**
 * Single byte status message send by the controller at the end of every
 * response. This message contains the overall request status, which could
 * indicate either success or error. */
struct __attribute__ ((packed)) ba_msg_status {
	uint8_t code;
};

struct __attribute__ ((packed)) ba_msg_event {
	/* bit-mask with events */
	enum ba_event mask;
};

struct __attribute__ ((packed)) ba_msg_device {

	/* device address */
	bdaddr_t addr;
	/* name obtained from the Bluetooth device itself */
	char name[32];

	/* determine whether battery is available */
	uint8_t battery:1;
	/* device battery level in range [0, 100] */
	uint8_t battery_level:7;

};

struct __attribute__ ((packed)) ba_msg_transport {

	/* device address for which the transport is created */
	bdaddr_t addr;

	/* selected profile and audio codec */
	enum ba_pcm_type type;
	enum ba_pcm_stream stream;
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

#endif
