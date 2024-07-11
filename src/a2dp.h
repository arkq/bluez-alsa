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

/**
 * Mapping between A2DP bit-field and its real value.
 *
 * When defining mapping array, all elements should be sorted from the
 * worst (least desired) to the best (most desired) values. This way,
 * the a2dp_foreach_get_best_*() functions can be used to find the best
 * possible value for the given bit-field. */
struct a2dp_bit_mapping {
	uint32_t bit_value;
	unsigned int value;
};

/**
 * Callback function for iterating over A2DP bit-field. */
typedef int (*a2dp_bit_mapping_foreach_func)(
		struct a2dp_bit_mapping mapping,
		void *userdata);

int a2dp_foreach_get_best_channel_mode(
		struct a2dp_bit_mapping mapping,
		void *userdata);
int a2dp_foreach_get_best_sampling_freq(
		struct a2dp_bit_mapping mapping,
		void *userdata);

int a2dp_bit_mapping_foreach(
		const struct a2dp_bit_mapping *mappings,
		uint32_t bitmask,
		a2dp_bit_mapping_foreach_func func,
		void *userdata);

unsigned int a2dp_bit_mapping_lookup(
		const struct a2dp_bit_mapping *mappings,
		uint32_t bit_value);

/**
 * A2DP Stream End-Point type. */
enum a2dp_type {
	A2DP_SOURCE = 0,
	A2DP_SINK = !A2DP_SOURCE,
};

/* XXX: avoid circular dependency */
struct ba_transport;

/**
 * A2DP Stream End-Point. */
struct a2dp_sep {

	enum a2dp_type type;
	uint32_t codec_id;
	const char *synopsis;

	/* capabilities configuration element */
	a2dp_t capabilities;
	size_t capabilities_size;

	/* callback function for SEP initialization */
	int (*init)(struct a2dp_sep *sep);

	/* callback function for codec-specific capabilities filtering; if this
	 * function is not provided, the a2dp_filter_capabilities() will return
	 * simple bitwise AND of given capabilities */
	int (*capabilities_filter)(
			const struct a2dp_sep *sep,
			const void *capabilities_mask,
			void *capabilities);

	/* callback function for selecting configuration */
	int (*configuration_select)(
			const struct a2dp_sep *sep,
			void *capabilities);

	/* callback function for checking configuration correctness */
	int (*configuration_check)(
			const struct a2dp_sep *sep,
			const void *configuration);

	int (*transport_init)(struct ba_transport *t);
	int (*transport_start)(struct ba_transport *t);

	/* D-Bus object path for external SEP */
	char bluez_dbus_path[64];
	/* determine whether SEP shall be enabled */
	bool enabled;

};

/* NULL-terminated list of available local A2DP SEPs */
extern struct a2dp_sep * const a2dp_seps[];

int a2dp_seps_init(void);

int a2dp_sep_cmp(const struct a2dp_sep *a, const struct a2dp_sep *b);
int a2dp_sep_ptr_cmp(const struct a2dp_sep **a, const struct a2dp_sep **b);

const struct a2dp_sep *a2dp_sep_lookup(
		enum a2dp_type type,
		uint32_t codec_id);

uint32_t a2dp_get_vendor_codec_id(
		const void *capabilities,
		size_t size);

int a2dp_filter_capabilities(
		const struct a2dp_sep *sep,
		const void *capabilities_mask,
		void *capabilities,
		size_t size);

int a2dp_select_configuration(
		const struct a2dp_sep *sep,
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
		const struct a2dp_sep *sep,
		const void *configuration,
		size_t size);

const char *a2dp_check_strerror(
		enum a2dp_check_err err);

int a2dp_transport_init(
		struct ba_transport *t);
int a2dp_transport_start(
		struct ba_transport *t);

#endif
