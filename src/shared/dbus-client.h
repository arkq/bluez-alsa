/*
 * BlueALSA - dbus-client.h
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_SHARED_DBUSCLIENT_H_
#define BLUEALSA_SHARED_DBUSCLIENT_H_

#include <poll.h>
#include <stddef.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <dbus/dbus.h>

#define BLUEALSA_SERVICE           "org.bluealsa"
#define BLUEALSA_INTERFACE_MANAGER "org.bluealsa.Manager1"
#define BLUEALSA_INTERFACE_PCM     "org.bluealsa.PCM1"
#define BLUEALSA_INTERFACE_RFCOMM  "org.bluealsa.RFCOMM1"

#define BA_PCM_TRANSPORT_A2DP_SOURCE (1 << 0)
#define BA_PCM_TRANSPORT_A2DP_SINK   (2 << 0)
#define BA_PCM_TRANSPORT_HFP_AG      (1 << 2)
#define BA_PCM_TRANSPORT_HFP_HF      (2 << 2)
#define BA_PCM_TRANSPORT_HSP_AG      (1 << 4)
#define BA_PCM_TRANSPORT_HSP_HS      (2 << 4)

#define BA_PCM_TRANSPORT_MASK_A2DP \
	(BA_PCM_TRANSPORT_A2DP_SOURCE | BA_PCM_TRANSPORT_A2DP_SINK)
#define BA_PCM_TRANSPORT_MASK_HFP \
	(BA_PCM_TRANSPORT_HFP_HF | BA_PCM_TRANSPORT_HFP_AG)
#define BA_PCM_TRANSPORT_MASK_HSP \
	(BA_PCM_TRANSPORT_HSP_HS | BA_PCM_TRANSPORT_HSP_AG)
#define BA_PCM_TRANSPORT_MASK_SCO \
	(BA_PCM_TRANSPORT_MASK_HFP | BA_PCM_TRANSPORT_MASK_HSP)
#define BA_PCM_TRANSPORT_MASK_AG \
	(BA_PCM_TRANSPORT_HSP_AG | BA_PCM_TRANSPORT_HFP_AG)
#define BA_PCM_TRANSPORT_MASK_HF \
	(BA_PCM_TRANSPORT_HSP_HS | BA_PCM_TRANSPORT_HFP_HF)

#define BA_PCM_MODE_SOURCE           (1 << 0)
#define BA_PCM_MODE_SINK             (1 << 1)

/**
 * Connection context. */
struct ba_dbus_ctx {
	/* private D-Bus connection */
	DBusConnection *conn;
	/* registered watches */
	DBusWatch **watches;
	size_t watches_len;
	/* registered matches */
	char **matches;
	size_t matches_len;
	/* BlueALSA service name */
	char ba_service[32];
};

/**
 * BlueALSA service property object. */
struct ba_service_props {
	/* service version */
	char version[32];
	/* currently used HCI adapters */
	char adapters[HCI_MAX_DEV][8];
};

/**
 * BlueALSA PCM object property. */
enum ba_pcm_property {
	BLUEALSA_PCM_SOFT_VOLUME,
	BLUEALSA_PCM_VOLUME,
};

/**
 * BlueALSA PCM object. */
struct ba_pcm {

	/* BlueZ D-Bus device path */
	char device_path[128];
	/* BlueALSA D-Bus PCM path */
	char pcm_path[128];

	/* connection sequence number */
	uint32_t sequence;

	/* BlueALSA transport type */
	unsigned int transport;
	/* stream mode */
	unsigned int mode;

	/* PCM stream format */
	dbus_uint16_t format;
	/* number of audio channels */
	unsigned char channels;
	/* PCM sampling frequency */
	dbus_uint32_t sampling;

	/* device address */
	bdaddr_t addr;
	/* transport codec */
	char codec[16];
	/* approximate PCM delay */
	dbus_uint16_t delay;
	/* software volume */
	dbus_bool_t soft_volume;

	/* 16-bit packed PCM volume */
	union {
		struct {
			dbus_uint16_t ch2_volume:7;
			dbus_uint16_t ch2_muted:1;
			dbus_uint16_t ch1_volume:7;
			dbus_uint16_t ch1_muted:1;
		};
		dbus_uint16_t raw;
	} volume;

};

dbus_bool_t bluealsa_dbus_connection_ctx_init(
		struct ba_dbus_ctx *ctx,
		const char *ba_service_name,
		DBusError *error);

void bluealsa_dbus_connection_ctx_free(
		struct ba_dbus_ctx *ctx);

dbus_bool_t bluealsa_dbus_connection_signal_match_add(
		struct ba_dbus_ctx *ctx,
		const char *sender,
		const char *path,
		const char *iface,
		const char *member,
		const char *extra);

dbus_bool_t bluealsa_dbus_connection_signal_match_clean(
		struct ba_dbus_ctx *ctx);

dbus_bool_t bluealsa_dbus_connection_dispatch(
		struct ba_dbus_ctx *ctx);

dbus_bool_t bluealsa_dbus_connection_poll_fds(
		struct ba_dbus_ctx *ctx,
		struct pollfd *fds,
		nfds_t *nfds);

dbus_bool_t bluealsa_dbus_connection_poll_dispatch(
		struct ba_dbus_ctx *ctx,
		struct pollfd *fds,
		nfds_t nfds);

dbus_bool_t bluealsa_dbus_get_props(
		struct ba_dbus_ctx *ctx,
		struct ba_service_props *props,
		DBusError *error);

dbus_bool_t bluealsa_dbus_get_pcms(
		struct ba_dbus_ctx *ctx,
		struct ba_pcm **pcms,
		size_t *length,
		DBusError *error);

dbus_bool_t bluealsa_dbus_get_pcm(
		struct ba_dbus_ctx *ctx,
		const bdaddr_t *addr,
		unsigned int transports,
		unsigned int mode,
		struct ba_pcm *pcm,
		DBusError *error);

dbus_bool_t bluealsa_dbus_open_pcm(
		struct ba_dbus_ctx *ctx,
		const char *pcm_path,
		int *fd_pcm,
		int *fd_pcm_ctrl,
		DBusError *error);

dbus_bool_t bluealsa_dbus_open_rfcomm(
		struct ba_dbus_ctx *ctx,
		const char *rfcomm_path,
		int *fd_rfcomm,
		DBusError *error);

dbus_bool_t bluealsa_dbus_pcm_update(
		struct ba_dbus_ctx *ctx,
		const struct ba_pcm *pcm,
		enum ba_pcm_property property,
		DBusError *error);

dbus_bool_t bluealsa_dbus_pcm_ctrl_send(
		int fd_pcm_ctrl,
		const char *command,
		DBusError *error);

#define bluealsa_dbus_pcm_ctrl_send_drain(fd, err) \
	bluealsa_dbus_pcm_ctrl_send(fd, "Drain", err)

#define bluealsa_dbus_pcm_ctrl_send_drop(fd, err) \
	bluealsa_dbus_pcm_ctrl_send(fd, "Drop", err)

#define bluealsa_dbus_pcm_ctrl_send_pause(fd, err) \
	bluealsa_dbus_pcm_ctrl_send(fd, "Pause", err)

#define bluealsa_dbus_pcm_ctrl_send_resume(fd, err) \
	bluealsa_dbus_pcm_ctrl_send(fd, "Resume", err)

dbus_bool_t bluealsa_dbus_message_iter_array_get_strings(
		DBusMessageIter *iter,
		DBusError *error,
		const char **strings,
		size_t *length);

dbus_bool_t bluealsa_dbus_message_iter_dict(
		DBusMessageIter *iter,
		DBusError *error,
		dbus_bool_t (*cb)(const char *key, DBusMessageIter *val, void *data, DBusError *err),
		void *userdata);

dbus_bool_t bluealsa_dbus_message_iter_get_pcm(
		DBusMessageIter *iter,
		DBusError *error,
		struct ba_pcm *pcm);

dbus_bool_t bluealsa_dbus_message_iter_get_pcm_props(
		DBusMessageIter *iter,
		DBusError *error,
		struct ba_pcm *pcm);

#endif
