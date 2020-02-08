/*
 * BlueALSA - rfcomm.h
 * Copyright (c) 2016-2019 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_RFCOMM_H_
#define BLUEALSA_RFCOMM_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdbool.h>
#include <stdint.h>

#include "at.h"
#include "ba-transport.h"
#include "hfp.h"

/* Timeout for the command acknowledgment. */
#define RFCOMM_TIMEOUT_ACK 1000
/* Timeout for the connection idle state. */
#define RFCOMM_TIMEOUT_IDLE 2500
/* Number of retries during the SLC stage. */
#define RFCOMM_SLC_RETRIES 10

/**
 * Structure used for RFCOMM state synchronization. */
struct rfcomm_conn {

	/* service level connection state */
	enum hfp_slc_state state;
	enum hfp_slc_state state_prev;

	/* initial connection setup */
	enum hfp_setup setup;

	/* handler used for sync response dispatching */
	const struct rfcomm_handler *handler;
	enum hfp_slc_state handler_resp_ok_new_state;
	bool handler_resp_ok_success;

	/* determine whether connection is idle */
	bool idle;

	/* number of failed communication attempts */
	int retries;

	/* requested codec by the AG */
	int codec;

	/* 0-based indicators index */
	enum hfp_ind hfp_ind_map[20];

	/* received event reporting setup */
	unsigned int hfp_cmer[5];

	/* variables used for AG<->HF sync */
	uint8_t gain_mic;
	uint8_t gain_spk;

#if ENABLE_MSBC
	/* determine whether mSBC is available */
	bool msbc;
#endif

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

void *rfcomm_thread(struct ba_transport *t);

#endif
