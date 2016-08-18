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

#include <gio/gio.h>

#define TRANSPORT_PROFILE_A2DP_SOURCE 0x01
#define TRANSPORT_PROFILE_A2DP_SINK   0x02
#define TRANSPORT_PROFILE_HFP         0x03
#define TRANSPORT_PROFILE_HSP         0x04

enum ba_transport_state {
	TRANSPORT_IDLE,
	TRANSPORT_PENDING,
	TRANSPORT_ACTIVE,
};

struct ba_transport {

	/* data required for D-Bus management */
	GDBusConnection *dbus_conn;
	char *dbus_owner;
	char *dbus_path;

	char *name;

	/* selected profile and audio codec */
	uint8_t profile;
	uint8_t codec;

	/* selected audio codec configuration */
	uint8_t *config;
	size_t config_size;

	/* software audio volume in range [0, 100] */
	uint8_t volume;
	/* if non-zero, equivalent of volume = 0 */
	uint8_t muted;

	/* IO thread - actual transport layer */
	enum ba_transport_state state;
	pthread_t thread;

	int bt_fd;
	size_t mtu_read;
	size_t mtu_write;

	char *pcm_fifo;
	int pcm_fd;

	/* callback functions for self-management */
	int (*acquire)(struct ba_transport *);
	int (*release)(struct ba_transport *);

};

struct ba_transport *transport_new(GDBusConnection *conn, const char *dbus_owner,
		const char *dbus_path, const char *name, uint8_t profile, uint8_t codec,
		const uint8_t *config, size_t config_size);
void transport_free(struct ba_transport *t);

int transport_set_state(struct ba_transport *t, enum ba_transport_state state);
int transport_set_state_from_string(struct ba_transport *t, const char *state);

int transport_acquire(struct ba_transport *t);
int transport_release(struct ba_transport *t);

#endif
