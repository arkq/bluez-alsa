/*
 * bluealsa - transport.h
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

#include <dbus/dbus.h>

enum ba_transport_type {
	TRANSPORT_DISABLED = 0,
	TRANSPORT_A2DP_SOURCE,
	TRANSPORT_A2DP_SINK,
	TRANSPORT_HFP,
	TRANSPORT_HSP,
	__TRANSPORT_MAX,
};

enum ba_transport_state {
	TRANSPORT_IDLE,
	TRANSPORT_PENDING,
	TRANSPORT_ACTIVE,
};

struct ba_transport {

	enum ba_transport_type type;
	char *name;

	/* data required for D-Bus management */
	DBusConnection *dbus_conn;
	char *dbus_owner;
	char *dbus_path;

	/* selected audio codec */
	uint8_t codec;
	uint8_t *config;
	size_t config_size;

	uint16_t volume;

	/* IO thread - actual transport layer */
	enum ba_transport_state state;
	pthread_t thread;

	int bt_fd;
	size_t mtu_read;
	size_t mtu_write;

	char *pcm_fifo;
	int pcm_fd;

#if 0
	uint16_t microphone_gain;
	uint16_t speaker_gain;

	// persistent stuff for encoding purpose
	uint16_t seq_num;   //cumulative packet number
	uint32_t timestamp; //timestamp
#endif

};


int transport_threads_init(void);

struct ba_transport *transport_new(enum ba_transport_type type, const char *name);
void transport_free(struct ba_transport *t);

int transport_set_dbus(struct ba_transport *t, DBusConnection *conn,
		const char *owner, const char *path);
int transport_set_codec(struct ba_transport *t, uint8_t codec,
		const uint8_t *config, size_t size);
int transport_set_state(struct ba_transport *t, enum ba_transport_state state);
int transport_set_state_from_string(struct ba_transport *t, const char *state);

int transport_acquire(struct ba_transport *t);
int transport_release(struct ba_transport *t);

const char *transport_type_to_string(enum ba_transport_type type);

#endif
