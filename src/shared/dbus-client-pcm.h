/*
 * BlueALSA - dbus-client-pcm.h
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_SHARED_DBUSCLIENTPCM_H_
#define BLUEALSA_SHARED_DBUSCLIENTPCM_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <endian.h>
#include <stddef.h>
#include <stdint.h>

#include <bluetooth/bluetooth.h>
#include <dbus/dbus.h>

#include "dbus-client.h"

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
 * BlueALSA PCM object property. */
enum ba_pcm_property {
	BLUEALSA_PCM_CLIENT_DELAY,
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
	/* number of channels supported by the codec */
	unsigned char channels[8];
	/* channel maps associated with supported number of channels */
	char channel_maps[8][8][5];
	/* sample rates supported by the codec */
	dbus_uint32_t rates[16];
};

/**
 * BlueALSA PCM codecs object. */
struct ba_pcm_codecs {
	struct ba_pcm_codec *codecs;
	size_t codecs_len;
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
	/* channel map for selected codec */
	char channel_map[8][5];
	/* PCM sample rate */
	dbus_uint32_t rate;

	/* device address */
	bdaddr_t addr;
	/* transport codec */
	struct ba_pcm_codec codec;
	/* approximate PCM delay */
	dbus_uint16_t delay;
	/* client delay */
	dbus_int16_t client_delay;
	/* software volume */
	dbus_bool_t soft_volume;

	/* per-channel volume */
	union {
		struct {
#if __BYTE_ORDER == __LITTLE_ENDIAN
			uint8_t volume:7;
			uint8_t muted:1;
#elif __BYTE_ORDER == __BIG_ENDIAN
			uint8_t muted:1;
			uint8_t volume:7;
#else
# error "Unknown byte order"
#endif
		};
		uint8_t raw;
	} volume[8];

};

dbus_bool_t ba_dbus_pcm_get_all(
		struct ba_dbus_ctx *ctx,
		struct ba_pcm **pcms,
		size_t *length,
		DBusError *error);

dbus_bool_t ba_dbus_pcm_get(
		struct ba_dbus_ctx *ctx,
		const bdaddr_t *addr,
		unsigned int transports,
		unsigned int mode,
		struct ba_pcm *pcm,
		DBusError *error);

dbus_bool_t ba_dbus_pcm_open(
		struct ba_dbus_ctx *ctx,
		const char *pcm_path,
		int *fd_pcm,
		int *fd_pcm_ctrl,
		DBusError *error);

const char *ba_dbus_pcm_codec_get_canonical_name(
		const char *alias);

dbus_bool_t ba_dbus_pcm_codecs_get(
		struct ba_dbus_ctx *ctx,
		const char *pcm_path,
		struct ba_pcm_codecs *codecs,
		DBusError *error);

void ba_dbus_pcm_codecs_free(
		struct ba_pcm_codecs *codecs);

#define BA_PCM_SELECT_CODEC_FLAG_NONE           (0)
#define BA_PCM_SELECT_CODEC_FLAG_NON_CONFORMANT (1 << 0)

dbus_bool_t ba_dbus_pcm_select_codec(
		struct ba_dbus_ctx *ctx,
		const char *pcm_path,
		const char *codec,
		const void *configuration,
		size_t configuration_len,
		unsigned int channels,
		unsigned int rate,
		unsigned int flags,
		DBusError *error);

dbus_bool_t ba_dbus_pcm_update(
		struct ba_dbus_ctx *ctx,
		const struct ba_pcm *pcm,
		enum ba_pcm_property property,
		DBusError *error);

dbus_bool_t ba_dbus_pcm_ctrl_send(
		int fd_pcm_ctrl,
		const char *command,
		int timeout,
		DBusError *error);

#define ba_dbus_pcm_ctrl_send_drain(fd, err) \
	ba_dbus_pcm_ctrl_send(fd, "Drain", 3000, err)

#define ba_dbus_pcm_ctrl_send_drop(fd, err) \
	ba_dbus_pcm_ctrl_send(fd, "Drop", 200, err)

#define ba_dbus_pcm_ctrl_send_pause(fd, err) \
	ba_dbus_pcm_ctrl_send(fd, "Pause", 200, err)

#define ba_dbus_pcm_ctrl_send_resume(fd, err) \
	ba_dbus_pcm_ctrl_send(fd, "Resume", 200, err)

dbus_bool_t dbus_message_iter_get_ba_pcm(
		DBusMessageIter *iter,
		DBusError *error,
		struct ba_pcm *pcm);

dbus_bool_t dbus_message_iter_get_ba_pcm_props(
		DBusMessageIter *iter,
		DBusError *error,
		struct ba_pcm *pcm);

#endif
