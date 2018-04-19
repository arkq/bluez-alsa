/*
 * BlueALSA - transport.h
 * Copyright (c) 2016-2018 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_TRANSPORT_H_
#define BLUEALSA_TRANSPORT_H_

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <glib.h>

#include "bluez.h"
#include "hfp.h"

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
	/* transport is in the eviction state */
	TRANSPORT_LIMBO,
};

enum ba_transport_signal {
	TRANSPORT_PCM_OPEN,
	TRANSPORT_PCM_CLOSE,
	TRANSPORT_PCM_PAUSE,
	TRANSPORT_PCM_RESUME,
	TRANSPORT_PCM_SYNC,
	TRANSPORT_SET_VOLUME,
	TRANSPORT_SEND_RFCOMM,
};

struct ba_device {

	/* ID of the underlying HCI device */
	int hci_dev_id;
	/* address of the Bluetooth device */
	bdaddr_t addr;
	/* human-readable Bluetooth device name */
	char name[HCI_MAX_NAME_LENGTH];

	/* adjusted (in the range 0-100) battery level */
	struct {
		bool enabled;
		uint8_t level;
	} battery;

	/* Apple's extension used with HFP profile */
	struct {

		uint16_t vendor_id;
		uint16_t product_id;
		uint16_t version;
		uint8_t features;

		/* determine whether headset is docked */
		uint8_t accev_docked;

	} xapl;

	/* hash-map with connected transports */
	GHashTable *transports;

};

struct ba_pcm {

	int fd;

	/* client identifier (most likely client socket file descriptor) used
	 * by the PCM client lookup function - transport_lookup_pcm_client() */
	int client;

	/* variables used for PCM synchronization */
	pthread_cond_t drained;
	pthread_mutex_t drained_mn;

};

struct ba_transport {

	/* backward reference to the owner */
	struct ba_device *device;

	/* Transport structure covers all transports supported by BlueALSA. However,
	 * every transport requires specific handling - link acquisition, transport
	 * specific configuration, freeing resources, etc. */
	enum ba_transport_type type;

	/* data required for D-Bus management */
	char *dbus_owner;
	char *dbus_path;

	/* Selected profile and audio codec. For A2DP vendor codecs the upper byte
	 * of the codec field contains the lowest byte of the vendor ID. */
	enum bluetooth_profile profile;
	uint16_t codec;

	/* IO thread - actual transport layer */
	enum ba_transport_state state;
	pthread_t thread;

	/* This field stores a file descriptor (socket) associated with the BlueZ
	 * side of the transport. The role of this socket depends on the transport
	 * type - it can be either A2DP, RFCOMM or SCO link. */
	int bt_fd;

	/* max transfer unit values for bt_fd */
	size_t mtu_read;
	size_t mtu_write;

	/* PIPE used to notify thread about changes. If thread is based on loop with
	 * an event wait syscall (e.g. poll), this file descriptor is used to send a
	 * control event. */
	int sig_fd[2];

	/* Overall delay in 1/10 of millisecond, caused by the data transfer and
	 * the audio encoder or decoder. */
	unsigned int delay;

	union {

		struct {

			/* if non-zero, equivalent of volume = 0 */
			uint8_t ch1_muted;
			uint8_t ch2_muted;
			/* software audio volume in range [0, 127] */
			uint8_t ch1_volume;
			uint8_t ch2_volume;

			/* delay reported by the AVDTP */
			uint16_t delay;

			struct ba_pcm pcm;

			/* selected audio codec configuration */
			uint8_t *cconfig;
			size_t cconfig_size;

		} a2dp;

		struct {

			/* associated SCO transport */
			struct ba_transport *sco;

			/* AG/HF supported features bitmask */
			uint32_t hfp_features;
			/* received AG indicator values */
			unsigned char hfp_inds[__HFP_IND_MAX];

		} rfcomm;

		struct {

			/* parent RFCOMM transport */
			struct ba_transport *rfcomm;

			/* if true, equivalent of gain = 0 */
			bool spk_muted;
			bool mic_muted;
			/* software audio gain in range [0, 15] */
			uint8_t spk_gain;
			uint8_t mic_gain;

			/* Speaker and microphone signals should to be exposed as
			 * a separate PCM devices. Hence, there is a requirement
			 * for separate configurations. */
			struct ba_pcm spk_pcm;
			struct ba_pcm mic_pcm;

		} sco;

	};

	/* callback function for self-management */
	int (*release)(struct ba_transport *);

};

struct ba_device *device_new(int hci_dev_id, const bdaddr_t *addr, const char *name);
void device_free(struct ba_device *d);

struct ba_device *device_get(GHashTable *devices, const char *key);
struct ba_device *device_lookup(GHashTable *devices, const char *key);
bool device_remove(GHashTable *devices, const char *key);

void device_set_battery_level(struct ba_device *d, uint8_t value);

struct ba_transport *transport_new(
		struct ba_device *device,
		enum ba_transport_type type,
		const char *dbus_owner,
		const char *dbus_path,
		enum bluetooth_profile profile,
		uint16_t codec);
struct ba_transport *transport_new_a2dp(
		struct ba_device *device,
		const char *dbus_owner,
		const char *dbus_path,
		enum bluetooth_profile profile,
		uint16_t codec,
		const uint8_t *config,
		size_t config_size);
struct ba_transport *transport_new_rfcomm(
		struct ba_device *device,
		const char *dbus_owner,
		const char *dbus_path,
		enum bluetooth_profile profile);
void transport_free(struct ba_transport *t);

struct ba_transport *transport_lookup(GHashTable *devices, const char *dbus_path);
struct ba_transport *transport_lookup_pcm_client(GHashTable *devices, int client);
bool transport_remove(GHashTable *devices, const char *dbus_path);

int transport_send_signal(struct ba_transport *t, enum ba_transport_signal sig);
int transport_send_rfcomm(struct ba_transport *t, const char command[32]);

unsigned int transport_get_channels(const struct ba_transport *t);
unsigned int transport_get_sampling(const struct ba_transport *t);

int transport_set_volume(struct ba_transport *t, uint8_t ch1_muted, uint8_t ch2_muted,
		uint8_t ch1_volume, uint8_t ch2_volume);

int transport_set_state(struct ba_transport *t, enum ba_transport_state state);
int transport_set_state_from_string(struct ba_transport *t, const char *state);

int transport_drain_pcm(struct ba_transport *t);

int transport_acquire_bt_a2dp(struct ba_transport *t);
int transport_release_bt_a2dp(struct ba_transport *t);

int transport_release_bt_rfcomm(struct ba_transport *t);

int transport_acquire_bt_sco(struct ba_transport *t);
int transport_release_bt_sco(struct ba_transport *t);

int transport_release_pcm(struct ba_pcm *pcm);

void transport_pthread_cleanup(void *arg);

#endif
