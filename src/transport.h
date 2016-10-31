/*
 * BlueALSA - transport.h
 * Copyright (c) 2016 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_TRANSPORT_H_
#define BLUEALSA_TRANSPORT_H_

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include <bluetooth/bluetooth.h>
#include <gio/gio.h>

#include "bluez.h"

enum ba_transport_type {
	TRANSPORT_TYPE_A2DP,
	TRANSPORT_TYPE_RFCOMM,
	TRANSPORT_TYPE_SCO,
};

enum ba_transport_state {
	TRANSPORT_IDLE,
	TRANSPORT_PENDING,
	TRANSPORT_ACTIVE,
	TRANSPORT_PAUSED,
	TRANSPORT_ABORTED,
};

#define TRANSPORT_RUN_IO_THREAD(t) \
	((t)->state == TRANSPORT_ACTIVE || (t)->state == TRANSPORT_PAUSED)

#define DEVICE_XAPL_FEATURE_BATTERY (1 << 1)
#define DEVICE_XAPL_FEATURE_DOCKING (1 << 2)
#define DEVICE_XAPL_FEATURE_SIRI    (1 << 3)
#define DEVICE_XAPL_FEATURE_DENOISE (1 << 4)

struct ba_device {

	/* ID of the underlying HCI device */
	int hci_dev_id;
	/* address of the Bluetooth device */
	bdaddr_t addr;
	char *name;

	/* Apple's extension used with HFP profile */
	struct {

		uint16_t vendor_id;
		uint16_t product_id;
		uint16_t version;
		uint8_t features;

		/* headset battery level in range [0, 9] */
		uint8_t accev_battery;
		/* determine whatever headset is docked */
		uint8_t accev_docked;

	} xapl;

	/* hash-map with connected transports */
	GHashTable *transports;

};

struct ba_transport {

	/* Transport structure covers all transports supported by BlueALSA. However,
	 * every transport requires specific handling - link acquisition, transport
	 * specific configuration, freeing resources, etc. */
	enum ba_transport_type type;

	/* data required for D-Bus management */
	GDBusConnection *dbus_conn;
	char *dbus_owner;
	char *dbus_path;

	/* backward reference to the owner */
	struct ba_device *device;

	char *name;

	/* selected profile and audio codec */
	enum bluetooth_profile profile;
	uint8_t codec;

	/* selected audio codec configuration */
	uint8_t *cconfig;
	size_t cconfig_size;

	/* if non-zero, equivalent of volume = 0 */
	uint8_t muted;
	/* software audio volume in range [0, 127] */
	uint8_t volume;

	/* IO thread - actual transport layer */
	enum ba_transport_state state;
	pthread_t thread;

	pthread_mutex_t resume_mutex;
	pthread_cond_t resume;

	int bt_fd;
	size_t mtu_read;
	size_t mtu_write;

	int rfcomm_fd;

	char *pcm_fifo;
	int pcm_fd;

	/* used by PCM client lookup */
	int pcm_client;

	/* callback function for self-management */
	int (*release)(struct ba_transport *);

};

struct ba_device *device_new(int hci_dev_id, const bdaddr_t *addr, const char *name);
void device_free(struct ba_device *d);

struct ba_device *device_get(GHashTable *devices, const char *key);
struct ba_device *device_lookup(GHashTable *devices, const char *key);
gboolean device_remove(GHashTable *devices, const char *key);

struct ba_transport *transport_new(enum ba_transport_type type,
		GDBusConnection *conn, const char *dbus_owner, const char *dbus_path,
		const char *name, enum bluetooth_profile profile, uint8_t codec,
		const uint8_t *config, size_t config_size);
void transport_free(struct ba_transport *t);

struct ba_transport *transport_lookup(GHashTable *devices, const char *key);
struct ba_transport *transport_lookup_pcm_client(GHashTable *devices, int client);
gboolean transport_remove(GHashTable *devices, const char *key);

unsigned int transport_get_channels(const struct ba_transport *t);
unsigned int transport_get_sampling(const struct ba_transport *t);

int transport_set_state(struct ba_transport *t, enum ba_transport_state state);
int transport_set_state_from_string(struct ba_transport *t, const char *state);

int transport_acquire_bt(struct ba_transport *t);
int transport_acquire_bt_sco(struct ba_transport *t);
int transport_release_bt(struct ba_transport *t);
int transport_release_bt_rfcomm(struct ba_transport *t);
int transport_release_pcm(struct ba_transport *t);

#endif
