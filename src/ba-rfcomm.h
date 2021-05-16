/*
 * BlueALSA - ba-rfcomm.h
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_BARFCOMM_H_
#define BLUEALSA_BARFCOMM_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "at.h"
#include "ba-transport.h"
#include "hfp.h"

/* Timeout for the command acknowledgment. */
#define BA_RFCOMM_TIMEOUT_ACK 1000
/* Timeout for the connection idle state. */
#define BA_RFCOMM_TIMEOUT_IDLE 2500
/* Number of retries during the SLC stage. */
#define BA_RFCOMM_SLC_RETRIES 10

enum ba_rfcomm_signal {
	BA_RFCOMM_SIGNAL_PING,
	BA_RFCOMM_SIGNAL_HFP_SET_CODEC_CVSD,
	BA_RFCOMM_SIGNAL_HFP_SET_CODEC_MSBC,
	BA_RFCOMM_SIGNAL_UPDATE_BATTERY,
	BA_RFCOMM_SIGNAL_UPDATE_VOLUME,
};

/**
 * Data associated with RFCOMM communication. */
struct ba_rfcomm {

	/* associated SCO transport */
	struct ba_transport *sco;

	/* RFCOMM socket */
	int fd;

	pthread_t thread;

	/* thread notification PIPE */
	int sig_fd[2];

	/* service level connection state */
	enum hfp_slc_state state;
	enum hfp_slc_state state_prev;

	/* initial connection setup */
	enum hfp_setup setup;

	/* handler used for sync response dispatching */
	const struct ba_rfcomm_handler *handler;
	enum hfp_slc_state handler_resp_ok_new_state;
	bool handler_resp_ok_success;

	/* external RFCOMM handler */
	int handler_fd;

	/* determine whether connection is idle */
	bool idle;

	/* number of failed communication attempts */
	int retries;

	/* AG/HF supported features bitmask */
	uint32_t hfp_features;

	/* codec selection synchronization */
	pthread_mutex_t codec_selection_completed_mtx;
	pthread_cond_t codec_selection_completed;

	/* requested codec by the AG */
	int codec;

	/* received AG indicator values */
	unsigned char hfp_ind[__HFP_IND_MAX];
	/* indicator activation state */
	bool hfp_ind_state[__HFP_IND_MAX];
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

	/* exported RFCOMM D-Bus API */
	char *ba_dbus_path;
	unsigned int ba_dbus_id;

	/* BlueZ does not trigger profile disconnection signal when the Bluetooth
	 * link has been lost (e.g. device power down). However, it is required to
	 * remove all references, otherwise resources will not be freed. If this
	 * quirk workaround is enabled, RFCOMM link lost will trigger SCO transport
	 * destroy rather than a simple unreferencing. */
	bool link_lost_quirk;

};

/**
 * Callback function used for RFCOMM AT message dispatching. */
typedef int ba_rfcomm_callback(struct ba_rfcomm *r, const struct bt_at *at);

/**
 * AT message dispatching handler. */
struct ba_rfcomm_handler {
	enum bt_at_type type;
	const char *command;
	ba_rfcomm_callback *callback;
};

struct ba_rfcomm *ba_rfcomm_new(struct ba_transport *sco, int fd);
void ba_rfcomm_destroy(struct ba_rfcomm *r);

int ba_rfcomm_send_signal(struct ba_rfcomm *r, enum ba_rfcomm_signal sig);

#endif
