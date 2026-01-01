/*
 * BlueALSA - ba-adapter.c
 * SPDX-FileCopyrightText: 2016-2025 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#include "ba-adapter.h"

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

#include "ba-config.h"
#include "ba-device.h"
#include "hci.h"
#include "hfp.h"
#include "utils.h"
#include "shared/log.h"
#include "shared/rc.h"

static void adapter_detach(void * ptr) {
	struct ba_adapter * a = ptr;
	/* Detach adapter from global configuration. */
	config.adapters[a->hci.dev_id] = NULL;
}

struct ba_adapter * ba_adapter_new(int dev_id) {

	/* make sure we are within array boundaries */
	if (dev_id < 0 || dev_id >= HCI_MAX_DEV)
		return errno = EINVAL, NULL;

	struct ba_adapter * a;
	if ((a = calloc(1, sizeof(*a))) == NULL)
		return NULL;

	/* Try to query for real information describing given HCI
	 * device. Upon failure, fill in some basic fields by hand. */
	if (hci_devinfo(dev_id, &a->hci) == -1) {
		warn("Couldn't get HCI device info: %s", strerror(errno));
		snprintf(a->hci.name, sizeof(a->hci.name), "hci%d", dev_id);
		a->hci.dev_id = dev_id;
	}

	/* Fill in the HCI version structure, which includes manufacturer
	 * ID. Note, that in order to get such info HCI has to be UP. */
	if (hci_get_version(dev_id, &a->chip) == -1)
		warn("Couldn't get HCI version: %s", strerror(errno));

	rc_init(&a->_rc, adapter_detach);

	sprintf(a->ba_dbus_path, "/org/bluealsa/%s", a->hci.name);
	g_variant_sanitize_object_path(a->ba_dbus_path);
	sprintf(a->bluez_dbus_path, "/org/bluez/%s", a->hci.name);
	g_variant_sanitize_object_path(a->bluez_dbus_path);

	pthread_mutex_init(&a->devices_mutex, NULL);
	a->devices = g_hash_table_new_full(g_bdaddr_hash, g_bdaddr_equal, NULL, NULL);

	pthread_mutex_lock(&config.adapters_mutex);
	config.adapters[a->hci.dev_id] = a;
	pthread_mutex_unlock(&config.adapters_mutex);

	return a;
}

struct ba_adapter * ba_adapter_lookup(int dev_id) {

	if (dev_id < 0 || dev_id >= HCI_MAX_DEV)
		return errno = EINVAL, NULL;

	struct ba_adapter * a;

	pthread_mutex_lock(&config.adapters_mutex);
	if ((a = config.adapters[dev_id]) != NULL)
		rc_ref(a);
	pthread_mutex_unlock(&config.adapters_mutex);

	return a;
}

void ba_adapter_destroy(struct ba_adapter * a) {

	if (a == NULL)
		return;

	/* XXX: Modification-safe remove-all loop.
	 *
	 * Before calling ba_device_destroy() we have to unlock mutex, so
	 * in theory it is possible that someone will modify devices hash
	 * table over which we are iterating. Since, the iterator uses an
	 * internal cache, we have to reinitialize it after every unlock. */
	for (;;) {

		GHashTableIter iter;
		struct ba_device * d;

		pthread_mutex_lock(&a->devices_mutex);

		g_hash_table_iter_init(&iter, a->devices);
		if (!g_hash_table_iter_next(&iter, NULL, (void *)&d)) {
			pthread_mutex_unlock(&a->devices_mutex);
			break;
		}

		ba_device_ref(d);
		g_hash_table_iter_steal(&iter);

		pthread_mutex_unlock(&a->devices_mutex);

		ba_device_destroy(d);
	}

	ba_adapter_unref(a);
}

void ba_adapter_unref(struct ba_adapter * a) {

	int ref_count;

	pthread_mutex_lock(&config.adapters_mutex);
	ref_count = rc_unref_with_count(a);
	pthread_mutex_unlock(&config.adapters_mutex);

	if (ref_count > 0)
		return;

	debug("Freeing adapter: %s", a->hci.name);
	g_assert_cmpint(ref_count, ==, 0);

	/* Make sure that the SCO dispatcher is terminated before free(). */
	if (a->sco_dispatcher != NULL) {
		g_source_destroy(a->sco_dispatcher);
		g_source_unref(a->sco_dispatcher);
	}

	g_hash_table_unref(a->devices);
	pthread_mutex_destroy(&a->devices_mutex);
	free(a);
}

/**
 * Get features exposed via RFCOMM for HFP-AG. */
unsigned int ba_adapter_get_hfp_features_ag(struct ba_adapter *a) {
	unsigned int features =
		HFP_AG_FEAT_REJECT |
		HFP_AG_FEAT_ECS |
		HFP_AG_FEAT_ECC;
	if (BA_TEST_ESCO_SUPPORT(a)) {
#if ENABLE_MSBC
		if (config.hfp.codecs.msbc)
			features |= HFP_AG_FEAT_CODEC;
#endif
#if ENABLE_LC3_SWB
		if (config.hfp.codecs.lc3_swb)
			features |= HFP_AG_FEAT_CODEC;
#endif
		features |= HFP_AG_FEAT_ESCO;
	}
	return features;
}

/**
 * Get features exposed via RFCOMM for HFP-HF. */
unsigned int ba_adapter_get_hfp_features_hf(struct ba_adapter *a) {
	unsigned int features =
		HFP_HF_FEAT_CLI |
		HFP_HF_FEAT_VOLUME |
		HFP_HF_FEAT_ECS |
		HFP_HF_FEAT_ECC;
	if (BA_TEST_ESCO_SUPPORT(a)) {
#if ENABLE_MSBC
		if (config.hfp.codecs.msbc)
			features |= HFP_HF_FEAT_CODEC;
#endif
#if ENABLE_LC3_SWB
		if (config.hfp.codecs.lc3_swb)
			features |= HFP_HF_FEAT_CODEC;
#endif
		features |= HFP_HF_FEAT_ESCO;
	}
	return features;
}
