/*
 * BlueALSA - a2dp.h
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_A2DP_H_
#define BLUEALSA_A2DP_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "shared/a2dp-codecs.h"

enum a2dp_dir {
	A2DP_SOURCE = 0,
	A2DP_SINK = !A2DP_SOURCE,
};

struct a2dp_channels {
	unsigned int count;
	uint16_t value;
};

struct a2dp_sampling {
	unsigned int frequency;
	uint16_t value;
};

/* XXX: avoid circular dependency */
struct ba_transport;

struct a2dp_codec {

	enum a2dp_dir dir;
	uint16_t codec_id;
	const char *synopsis;

	/* capabilities configuration element */
	a2dp_t capabilities;
	size_t capabilities_size;

	/* callback function for codec initialization */
	int (*init)(struct a2dp_codec *codec);

	/* callback function for codec-specific capabilities filtering; if this
	 * function is not provided, the a2dp_filter_capabilities() will return
	 * simple bitwise AND of given capabilities */
	int (*capabilities_filter)(
			const struct a2dp_codec *codec,
			const void *capabilities_mask,
			void *capabilities);

	/* callback function for selecting configuration */
	int (*configuration_select)(
			const struct a2dp_codec *codec,
			void *capabilities);

	/* callback function for checking configuration correctness */
	int (*configuration_check)(
			const struct a2dp_codec *codec,
			const void *configuration);

	int (*transport_init)(struct ba_transport *t);
	int (*transport_start)(struct ba_transport *t);

	/* determine whether codec shall be enabled */
	bool enabled;

};

/**
 * A2DP Stream End-Point. */
struct a2dp_sep {
	enum a2dp_dir dir;
	uint16_t codec_id;
	/* exposed capabilities */
	a2dp_t capabilities;
	size_t capabilities_size;
	/* stream end-point path */
	char bluez_dbus_path[64];
	/* selected configuration */
	a2dp_t configuration;
};

/* NULL-terminated list of available A2DP codecs */
extern struct a2dp_codec * const a2dp_codecs[];

int a2dp_codecs_init(void);

int a2dp_codec_cmp(const struct a2dp_codec *a, const struct a2dp_codec *b);
int a2dp_codec_ptr_cmp(const struct a2dp_codec **a, const struct a2dp_codec **b);
int a2dp_sep_cmp(const struct a2dp_sep *a, const struct a2dp_sep *b);

const struct a2dp_codec *a2dp_codec_lookup(
		uint16_t codec_id,
		enum a2dp_dir dir);

const struct a2dp_channels *a2dp_channels_lookup(
		const struct a2dp_channels *channels,
		uint16_t value);

const struct a2dp_channels *a2dp_channels_select(
		const struct a2dp_channels *channels,
		uint16_t capabilities);

const struct a2dp_sampling *a2dp_sampling_lookup(
		const struct a2dp_sampling *samplings,
		uint16_t value);

const struct a2dp_sampling *a2dp_sampling_select(
		const struct a2dp_sampling *samplings,
		uint16_t capabilities);

uint16_t a2dp_get_vendor_codec_id(
		const void *capabilities,
		size_t size);

int a2dp_filter_capabilities(
		const struct a2dp_codec *codec,
		const void *capabilities_mask,
		void *capabilities,
		size_t size);

int a2dp_select_configuration(
		const struct a2dp_codec *codec,
		void *capabilities,
		size_t size);

enum a2dp_check_err {
	A2DP_CHECK_OK = 0,
	A2DP_CHECK_ERR_SIZE,
	A2DP_CHECK_ERR_CHANNEL_MODE,
	A2DP_CHECK_ERR_SAMPLING,
	A2DP_CHECK_ERR_ALLOCATION_METHOD,
	A2DP_CHECK_ERR_BIT_POOL_RANGE,
	A2DP_CHECK_ERR_SUB_BANDS,
	A2DP_CHECK_ERR_BLOCK_LENGTH,
	A2DP_CHECK_ERR_MPEG_LAYER,
	A2DP_CHECK_ERR_OBJECT_TYPE,
	A2DP_CHECK_ERR_DIRECTIONS,
	A2DP_CHECK_ERR_SAMPLING_VOICE,
	A2DP_CHECK_ERR_SAMPLING_MUSIC,
	A2DP_CHECK_ERR_FRAME_DURATION,
};

enum a2dp_check_err a2dp_check_configuration(
		const struct a2dp_codec *codec,
		const void *configuration,
		size_t size);

const char *a2dp_check_strerror(
		enum a2dp_check_err err);

int a2dp_transport_init(
		struct ba_transport *t);
int a2dp_transport_start(
		struct ba_transport *t);

#endif
