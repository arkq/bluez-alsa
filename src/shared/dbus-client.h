/*
 * BlueALSA - dbus-client.h
 * Copyright (c) 2016-2022 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_SHARED_DBUSCLIENT_H_
#define BLUEALSA_SHARED_DBUSCLIENT_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <endian.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <dbus/dbus.h>

#ifndef DBUS_INTERFACE_OBJECT_MANAGER
# define DBUS_INTERFACE_OBJECT_MANAGER "org.freedesktop.DBus.ObjectManager"
#endif

#define BLUEALSA_SERVICE           "org.bluealsa"
#define BLUEALSA_INTERFACE_MANAGER "org.bluealsa.Manager1"
#define BLUEALSA_INTERFACE_PCM     "org.bluealsa.PCM1"
#define BLUEALSA_INTERFACE_RFCOMM  "org.bluealsa.RFCOMM1"

#define BA_PCM_TRANSPORT_NONE        (0)
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
 * Determine whether given PCM is transported
 * over A2DP codec main-channel link. */
#define BA_PCM_A2DP_MAIN_CHANNEL(pcm) ( \
       ((pcm)->transport & BA_PCM_TRANSPORT_A2DP_SOURCE && (pcm)->mode & BA_PCM_MODE_SINK) || \
       ((pcm)->transport & BA_PCM_TRANSPORT_A2DP_SINK && (pcm)->mode & BA_PCM_MODE_SOURCE))

/**
 * Determine whether given PCM is transported
 * over HFP/HSP speaker channel link. */
#define BA_PCM_SCO_SPEAKER_CHANNEL(pcm) ( \
				((pcm)->transport & BA_PCM_TRANSPORT_MASK_AG && (pcm)->mode & BA_PCM_MODE_SINK) || \
				((pcm)->transport & BA_PCM_TRANSPORT_MASK_HF && (pcm)->mode & BA_PCM_MODE_SOURCE))

/**
 * Get max volume level for given PCM. */
#define BA_PCM_VOLUME_MAX(pcm) \
	((pcm)->transport & BA_PCM_TRANSPORT_MASK_A2DP ? 127 : 15)

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
	size_t adapters_len;
	/* currently used Bluetooth profiles */
	char **profiles;
	size_t profiles_len;
	/* currently used audio codecs */
	char **codecs;
	size_t codecs_len;
};

/**
 * BlueALSA RFCOMM property object. */
struct ba_rfcomm_props {
	/* BlueALSA transport type */
	char transport[7];
	/* remote device supported features */
	char **features;
	size_t features_len;
	/* remote device battery level */
	int battery;
};

/**
 * BlueALSA PCM object property. */
enum ba_pcm_property {
	BLUEALSA_PCM_SOFT_VOLUME,
	BLUEALSA_PCM_VOLUME,
};

/**
 * BlueALSA PCM codec object. */
struct ba_pcm_codec {
	/* codec canonical name */
	char name[16];
	/* Data associated with the codec. For A2DP transport it might
	 * be a capabilities or a configuration blob respectively for
	 * the list of available codecs or currently selected codec. */
	uint8_t data[24];
	size_t data_len;
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
	/* transport running */
	dbus_bool_t running;

	/* PCM stream format */
	dbus_uint16_t format;
	/* number of audio channels */
	unsigned char channels;
	/* PCM sampling frequency */
	dbus_uint32_t sampling;

	/* device address */
	bdaddr_t addr;
	/* transport codec */
	struct ba_pcm_codec codec;
	/* approximate PCM delay */
	dbus_uint16_t delay;
	/* manual delay adjustment */
	dbus_int16_t delay_adjustment;
	/* software volume */
	dbus_bool_t soft_volume;

	/* 16-bit packed PCM volume */
	union {
		struct {
#if __BYTE_ORDER == __LITTLE_ENDIAN
			dbus_uint16_t ch2_volume:7;
			dbus_uint16_t ch2_muted:1;
			dbus_uint16_t ch1_volume:7;
			dbus_uint16_t ch1_muted:1;
#elif __BYTE_ORDER == __BIG_ENDIAN
			dbus_uint16_t ch1_muted:1;
			dbus_uint16_t ch1_volume:7;
			dbus_uint16_t ch2_muted:1;
			dbus_uint16_t ch2_volume:7;
#else
# error "Unknown byte order"
#endif
		};
		dbus_uint16_t raw;
	} volume;

};

/**
 * BlueALSA PCM codecs object. */
struct ba_pcm_codecs {
	struct ba_pcm_codec *codecs;
	size_t codecs_len;
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

void bluealsa_dbus_props_free(
		struct ba_service_props *props);

dbus_bool_t bluealsa_dbus_get_rfcomm_props(
		struct ba_dbus_ctx *ctx,
		const char *rfcomm_path,
		struct ba_rfcomm_props *props,
		DBusError *error);

void bluealsa_dbus_rfcomm_props_free(
		struct ba_rfcomm_props *props);

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

dbus_bool_t bluealsa_dbus_pcm_open(
		struct ba_dbus_ctx *ctx,
		const char *pcm_path,
		int *fd_pcm,
		int *fd_pcm_ctrl,
		DBusError *error);

const char *bluealsa_dbus_pcm_get_codec_canonical_name(
		const char *alias);

dbus_bool_t bluealsa_dbus_pcm_get_codecs(
		struct ba_dbus_ctx *ctx,
		const char *pcm_path,
		struct ba_pcm_codecs *codecs,
		DBusError *error);

void bluealsa_dbus_pcm_codecs_free(
		struct ba_pcm_codecs *codecs);

dbus_bool_t bluealsa_dbus_pcm_select_codec(
		struct ba_dbus_ctx *ctx,
		const char *pcm_path,
		const char *codec,
		const void *configuration,
		size_t configuration_len,
		DBusError *error);

dbus_bool_t bluealsa_dbus_pcm_set_delay_adjustment(
		struct ba_dbus_ctx *ctx,
		const char *pcm_path,
		const char *codec,
		int16_t adjustment,
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
		int timeout,
		DBusError *error);

#define bluealsa_dbus_pcm_ctrl_send_drain(fd, err) \
	bluealsa_dbus_pcm_ctrl_send(fd, "Drain", 3000, err)

#define bluealsa_dbus_pcm_ctrl_send_drop(fd, err) \
	bluealsa_dbus_pcm_ctrl_send(fd, "Drop", 200, err)

#define bluealsa_dbus_pcm_ctrl_send_pause(fd, err) \
	bluealsa_dbus_pcm_ctrl_send(fd, "Pause", 200, err)

#define bluealsa_dbus_pcm_ctrl_send_resume(fd, err) \
	bluealsa_dbus_pcm_ctrl_send(fd, "Resume", 200, err)

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
