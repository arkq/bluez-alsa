/*
 * BlueALSA - ctl-proto.h
 * Copyright (c) 2016-2019 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_SHARED_CTLPROTO_H_
#define BLUEALSA_SHARED_CTLPROTO_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdint.h>
#include <bluetooth/bluetooth.h>

/* Location where the control socket and pipes are stored. */
#define BLUEALSA_RUN_STATE_DIR RUN_STATE_DIR "/bluealsa"
/* Version of the controller communication protocol. */
#define BLUEALSA_CRL_PROTO_VERSION 0x0500

enum ba_command {
	BA_COMMAND_PING,
	BA_COMMAND_SUBSCRIBE,
	BA_COMMAND_LIST_DEVICES,
	BA_COMMAND_LIST_TRANSPORTS,
	BA_COMMAND_TRANSPORT_GET,
	BA_COMMAND_TRANSPORT_SET_DELAY,
	BA_COMMAND_TRANSPORT_SET_VOLUME,
	BA_COMMAND_PCM_OPEN,
	BA_COMMAND_PCM_PAUSE,
	BA_COMMAND_PCM_RESUME,
	BA_COMMAND_PCM_DRAIN,
	BA_COMMAND_PCM_DROP,
	BA_COMMAND_RFCOMM_SEND,
	__BA_COMMAND_MAX
};

enum ba_status_code {
	BA_STATUS_CODE_SUCCESS = 0,
	BA_STATUS_CODE_ERROR_UNKNOWN,
	BA_STATUS_CODE_DEVICE_NOT_FOUND,
	BA_STATUS_CODE_STREAM_NOT_FOUND,
	BA_STATUS_CODE_CODEC_NOT_SELECTED,
	BA_STATUS_CODE_DEVICE_BUSY,
	BA_STATUS_CODE_FORBIDDEN,
};

enum ba_event {
	BA_EVENT_TRANSPORT_ADDED   = 1 << 0,
	BA_EVENT_TRANSPORT_CHANGED = 1 << 1,
	BA_EVENT_TRANSPORT_REMOVED = 1 << 2,
	BA_EVENT_VOLUME_CHANGED    = 1 << 3,
	BA_EVENT_BATTERY           = 1 << 4,
};

enum ba_pcm_type {
	BA_PCM_TYPE_NULL = 0,
	BA_PCM_TYPE_A2DP,
	BA_PCM_TYPE_SCO,
};

#define BA_PCM_STREAM_PLAYBACK (1 << 6)
#define BA_PCM_STREAM_CAPTURE  (1 << 7)

/**
 * Bit mask for extracting actual PCM type enum value from the
 * transport type field defined in the message structures. */
#define BA_PCM_TYPE_MASK 0x3F

/**
 * Extract PCM type enum from the given value. */
#define BA_PCM_TYPE(v) ((v) & BA_PCM_TYPE_MASK)

struct __attribute__ ((packed)) ba_request {

	enum ba_command command;

	/* selected device address */
	bdaddr_t addr;
	/* selected transport type */
	uint8_t type;

	union {

		/* bit-mask with event subscriptions
		 * used by BA_COMMAND_SUBSCRIBE */
		enum ba_event events;

		/* transport delay
		 * used by BA_COMMAND_TRANSPORT_SET_DELAY */
		uint16_t delay;

		/* transport volume fields
		 * used by BA_COMMAND_TRANSPORT_SET_VOLUME */
		struct {
			uint8_t ch1_muted:1;
			uint8_t ch1_volume:7;
			uint8_t ch2_muted:1;
			uint8_t ch2_volume:7;
		};

		/* RFCOMM command string to send
		 * used by BA_COMMAND_RFCOMM_SEND */
		char rfcomm_command[32];

	};

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
	uint8_t events;
	/* device address for which event occurred */
	bdaddr_t addr;
	/* transport type for which event occurred */
	uint8_t type;
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

	/* device address */
	bdaddr_t addr;
	/* transport type */
	uint8_t type;
	/* selected audio codec */
	uint16_t codec;

	/* number of audio channels */
	uint8_t channels;
	/* used sampling frequency */
	uint32_t sampling;

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
