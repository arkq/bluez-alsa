/*
 * BlueALSA - a2dp.h
 * SPDX-FileCopyrightText: 2016-2025 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BLUEALSA_A2DP_H_
#define BLUEALSA_A2DP_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "ba-transport-pcm.h"
#include "error.h"
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
	union {
		/* Single value mapping. */
		unsigned int value;
		/* Channel mode mapping with channel count and channel map. */
		struct {
			unsigned int channels;
			const enum ba_transport_pcm_channel *map;
		} ch;
	};
};

static_assert(
	offsetof(struct a2dp_bit_mapping, value) ==
	offsetof(struct a2dp_bit_mapping, ch.channels),
	"Invalid a2dp_bit_mapping structure layout");

/**
 * Callback function for iterating over A2DP bit-field. */
typedef error_code_t (*a2dp_bit_mapping_foreach_func)(
		struct a2dp_bit_mapping mapping,
		void *userdata);

extern const enum ba_transport_pcm_channel a2dp_channel_map_mono[];
extern const enum ba_transport_pcm_channel a2dp_channel_map_stereo[];
extern const enum ba_transport_pcm_channel a2dp_channel_map_5_1[];
extern const enum ba_transport_pcm_channel a2dp_channel_map_7_1[];

error_code_t a2dp_bit_mapping_foreach_get_best_channel_mode(
		struct a2dp_bit_mapping mapping,
		void *userdata);
error_code_t a2dp_bit_mapping_foreach_get_best_sample_rate(
		struct a2dp_bit_mapping mapping,
		void *userdata);

error_code_t a2dp_bit_mapping_foreach(
		const struct a2dp_bit_mapping *mappings,
		uint32_t bitmask,
		a2dp_bit_mapping_foreach_func func,
		void *userdata);

ssize_t a2dp_bit_mapping_lookup(
		const struct a2dp_bit_mapping *mappings,
		uint32_t bit_value);

uint32_t a2dp_bit_mapping_lookup_value(
		const struct a2dp_bit_mapping *mappings,
		uint32_t bitmask,
		unsigned int value);

/**
 * A2DP stream direction. */
enum a2dp_stream {
	A2DP_MAIN,
	A2DP_BACKCHANNEL,
};

void a2dp_caps_bitwise_intersect(
		void * restrict capabilities,
		const void * restrict mask,
		size_t size);

bool a2dp_caps_has_main_stream_only(
		const void *capabilities,
		enum a2dp_stream stream);

/* A2DP capabilities helper functions. */
struct a2dp_caps_helpers {

	/**
	 * Function for codec-specific capabilities capping. */
	void (*intersect)(
			void *capabilities,
			const void *mask);

	/**
	 * Function for checking whether given stream direction is supported. */
	bool (*has_stream)(
			const void *capabilities,
			enum a2dp_stream stream);

	/* Function for iterating over channel modes. */
	error_code_t (*foreach_channel_mode)(
			const void *capabilities,
			enum a2dp_stream stream,
			a2dp_bit_mapping_foreach_func func,
			void *userdata);

	/* Function for iterating over sample rates. */
	error_code_t (*foreach_sample_rate)(
			const void *capabilities,
			enum a2dp_stream stream,
			a2dp_bit_mapping_foreach_func func,
			void *userdata);

	void (*select_channel_mode)(
			void *capabilities,
			enum a2dp_stream stream,
			unsigned int channels);

	void (*select_sample_rate)(
			void *capabilities,
			enum a2dp_stream stream,
			unsigned int rate);

};

/**
 * A2DP Stream End-Point type. */
enum a2dp_type {
	A2DP_SOURCE = 0,
	A2DP_SINK = !A2DP_SOURCE,
};

/* A2DP Stream End-Point configuration. */
struct a2dp_sep_config {

	/* SEP type */
	enum a2dp_type type;
	/* extended (32-bit) codec ID */
	uint32_t codec_id;

	/* supported capabilities */
	a2dp_t capabilities;
	size_t caps_size;

	/* D-Bus object path for external SEP */
	char bluez_dbus_path[64];

};

/**
 * A2DP Stream End-Point. */
struct a2dp_sep {

	const char *name;
	struct a2dp_sep_config config;

	/* callback function for SEP initialization */
	error_code_t (*init)(struct a2dp_sep *sep);

	/* callback function for selecting configuration */
	error_code_t (*configuration_select)(
			const struct a2dp_sep *sep,
			void *capabilities);

	/* callback function for checking configuration correctness */
	error_code_t (*configuration_check)(
			const struct a2dp_sep *sep,
			const void *configuration);

	int (*transport_init)(struct ba_transport *t);
	int (*transport_start)(struct ba_transport *t);

	/* Codec-specific capabilities helper functions. */
	const struct a2dp_caps_helpers *caps_helpers;

	/* determine whether SEP shall be enabled */
	bool enabled;

};

/* NULL-terminated list of available local A2DP SEPs */
extern struct a2dp_sep * const a2dp_seps[];

error_code_t a2dp_seps_init(void);

int a2dp_sep_config_cmp(
		const struct a2dp_sep_config *a,
		const struct a2dp_sep_config *b);
int a2dp_sep_ptr_cmp(
		const struct a2dp_sep **a,
		const struct a2dp_sep **b);

const struct a2dp_sep *a2dp_sep_lookup(
		enum a2dp_type type,
		uint32_t codec_id);

uint32_t a2dp_get_vendor_codec_id(
		const void *capabilities,
		size_t size);

error_code_t a2dp_select_configuration(
		const struct a2dp_sep *sep,
		void *capabilities,
		size_t size);

error_code_t a2dp_check_configuration(
		const struct a2dp_sep *sep,
		const void *configuration,
		size_t size);

#endif
