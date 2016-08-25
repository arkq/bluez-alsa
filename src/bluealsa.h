/*
 * BlueALSA - bluealsa.h
 * Copyright (c) 2016 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_BLUEALSA_H
#define BLUEALSA_BLUEALSA_H

#include <pthread.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <glib.h>

struct ba_setup {

	/* used HCI device */
	struct hci_dev_info hci_dev;

	gboolean enable_a2dp;
	gboolean enable_hsp;

	/* collection of connected devices */
	pthread_mutex_t devices_mutex;
	GHashTable *devices;

};

int bluealsa_setup_init(struct ba_setup *setup);
void bluealsa_setup_free(struct ba_setup *setup);

#endif
