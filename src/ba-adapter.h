/*
 * BlueALSA - ba-adapter.h
 * SPDX-FileCopyrightText: 2016-2025 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#ifndef BLUEALSA_BAADAPTER_H_
#define BLUEALSA_BAADAPTER_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <pthread.h>

#include <glib.h>

#include <bluetooth/bluetooth.h> /* IWYU pragma: keep */
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

#include "shared/rc.h"

/* Data associated with BT adapter. */
struct ba_adapter {
	rc_t _rc;

	/* Basic info about HCI device. */
	struct hci_dev_info hci;
	struct hci_version chip;

	/* Dispatcher for incoming SCO connections. */
	GSource * sco_dispatcher;

	/* Data for D-Bus management. */
	char ba_dbus_path[32];
	char bluez_dbus_path[32];

	/* Collection of connected devices. */
	pthread_mutex_t devices_mutex;
	GHashTable * devices;

};

struct ba_adapter * ba_adapter_new(int dev_id);
struct ba_adapter * ba_adapter_lookup(int dev_id);
static inline struct ba_adapter * ba_adapter_ref(
		struct ba_adapter * a) {
	return rc_ref(a);
}

void ba_adapter_destroy(struct ba_adapter * a);
void ba_adapter_unref(struct ba_adapter * a);

/**
 * Macro for testing whether eSCO is supported. */
#define BA_TEST_ESCO_SUPPORT(a) \
	((a)->hci.features[2] & LMP_TRSP_SCO && (a)->hci.features[3] & LMP_ESCO)

unsigned int ba_adapter_get_hfp_features_ag(struct ba_adapter *a);
unsigned int ba_adapter_get_hfp_features_hf(struct ba_adapter *a);

#endif
