/*
 * BlueALSA - ba-adapter.h
 * Copyright (c) 2016-2019 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_BAADAPTER_H_
#define BLUEALSA_BAADAPTER_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <pthread.h>

#include <glib.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

/* Data associated with BT adapter. */
struct ba_adapter {

	/* basic info about HCI device */
	struct hci_dev_info hci;
	struct hci_version chip;

	/* incoming SCO links dispatcher */
	pthread_t sco_dispatcher;

	/* data for D-Bus management */
	char ba_dbus_path[32];
	char bluez_dbus_path[32];

	/* collection of connected devices */
	pthread_mutex_t devices_mutex;
	GHashTable *devices;

	/* memory self-management */
	int ref_count;

};

struct ba_adapter *ba_adapter_new(int dev_id);
struct ba_adapter *ba_adapter_lookup(int dev_id);
struct ba_adapter *ba_adapter_ref(struct ba_adapter *a);
void ba_adapter_destroy(struct ba_adapter *a);
void ba_adapter_unref(struct ba_adapter *a);

/**
 * Macro for testing whether eSCO is supported. */
#define BA_TEST_ESCO_SUPPORT(a) \
	((a)->hci.features[2] & LMP_TRSP_SCO && (a)->hci.features[3] & LMP_ESCO)

int ba_adapter_get_hfp_features_hf(struct ba_adapter *a);
int ba_adapter_get_hfp_features_ag(struct ba_adapter *a);

#endif
