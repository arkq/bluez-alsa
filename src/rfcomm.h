/*
 * BlueALSA - rfcomm.h
 * Copyright (c) 2016-2018 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_RFCOMM_H_
#define BLUEALSA_RFCOMM_H_

#include "at.h"
#include "hfp.h"
#include "transport.h"

/* Number of retries during the SLC stage. */
#define RFCOMM_SLC_RETRIES 10
/* Timeout for the command acknowledgment. */
#define RFCOMM_SLC_TIMEOUT 1000

/**
 * Structure used for RFCOMM state synchronization. */
struct rfcomm_conn {

	/* service level connection state */
	enum hfp_state state;
	enum hfp_state state_prev;

	/* handler used for sync response dispatching */
	const struct rfcomm_handler *handler;

	/* number of failed communication attempts */
	int retries;

	/* 0-based indicators index */
	enum hfp_ind hfp_ind_map[20];

	/* variables used for AG<->HF sync */
	uint8_t spk_gain;
	uint8_t mic_gain;

	/* associated transport */
	struct ba_transport *t;

};

/**
 * Callback function used for RFCOMM AT message dispatching. */
typedef int rfcomm_callback(struct rfcomm_conn *c, const struct bt_at *at);

/**
 * AT message dispatching handler. */
struct rfcomm_handler {
	enum bt_at_type type;
	const char *command;
	rfcomm_callback *callback;
};

void *rfcomm_thread(void *arg);

#endif
