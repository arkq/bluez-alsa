/*
 * BlueALSA - rfcomm.h
 * Copyright (c) 2016-2017 Arkadiusz Bokowy
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

#define BA_HFP_AG_FEATURES (\
		HFP_AG_FEAT_REJECT |\
		HFP_AG_FEAT_ECS |\
		HFP_AG_FEAT_ECC |\
		HFP_AG_FEAT_EERC |\
		HFP_AG_FEAT_CODEC)

#define BA_HFP_HF_FEATURES (\
		HFP_HF_FEAT_CLI |\
		HFP_HF_FEAT_VOLUME |\
		HFP_HF_FEAT_ECS |\
		HFP_HF_FEAT_ECC |\
		HFP_HF_FEAT_CODEC)

/**
 * Callback function used for RFCOMM AT message dispatching. */
typedef int rfcomm_callback(struct ba_transport *t, struct bt_at *at, int fd);

void *rfcomm_thread(void *arg);

#endif
