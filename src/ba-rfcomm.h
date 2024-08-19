/*
 * BlueALSA - ba-rfcomm.h
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
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
#include <stdatomic.h>
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
	BA_RFCOMM_SIGNAL_HFP_SET_CODEC_LC3_SWB,
	BA_RFCOMM_SIGNAL_UPDATE_BATTERY,
	BA_RFCOMM_SIGNAL_UPDATE_VOLUME,
};

struct ba_rfcomm_hfp_codecs {
	bool cvsd;
#if ENABLE_MSBC
	bool msbc;
#endif
#if ENABLE_LC3_SWB
	bool lc3_swb;
#endif
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

	/* AG supported feature mask */
	uint32_t ag_features;
	/* HF supported feature mask */
	uint32_t hf_features;

	/* AG supported codecs */
	struct ba_rfcomm_hfp_codecs ag_codecs;
	/* HF supported codecs */
	struct ba_rfcomm_hfp_codecs hf_codecs;

	/* HF supported codecs encoded for BAC command and BCS error response. */
	char hf_bac_bcs_string[8];

	/* Synchronization primitives for codec selection. The condition variable
	 * shall be used with the codec_id mutex from the associated transport. */
	pthread_cond_t codec_selection_cond;
	bool codec_selection_done;

	/* requested codec by the AG */
	uint8_t codec_id;

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

	/* exported RFCOMM D-Bus API */
	char *ba_dbus_path;
	bool ba_dbus_exported;

	/* BlueZ does not trigger profile disconnection signal when the Bluetooth
	 * link has been lost (e.g. device power down). However, it is required to
	 * remove all references, otherwise resources will not be freed. If this
	 * quirk workaround is enabled, RFCOMM link lost will trigger SCO transport
	 * destroy rather than a simple unreferencing. */
	atomic_bool link_lost_quirk;

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
