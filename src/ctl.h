/*
 * BlueALSA - ctl.h
 * Copyright (c) 2016-2019 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_CTL_H_
#define BLUEALSA_CTL_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include <bluetooth/bluetooth.h>

#include <glib.h>

#include "shared/ctl-proto.h"

struct ba_ctl {

	pthread_t thread;
	bool socket_created;
	bool thread_created;

	/* associated BT adapter */
	struct ba_adapter *a;

	/* special file descriptors + connected clients */
	GArray *pfds;
	/* event subscriptions for connected clients */
	GArray *subs;

	/* PIPE for transferring events */
	int evt[2];

};

struct ba_ctl *bluealsa_ctl_init(struct ba_adapter *adapter);
void bluealsa_ctl_free(struct ba_ctl *ctl);

int bluealsa_ctl_send_event(
		struct ba_ctl *ctl,
		enum ba_event event,
		const bdaddr_t *addr,
		uint8_t type);

#endif
