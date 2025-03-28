/*
 * BlueALSA - alsa-mixer.h
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_APLAY_ALSAMIXER_H_
#define BLUEALSA_APLAY_ALSAMIXER_H_

#include <poll.h>
#include <stdbool.h>
#include <stddef.h>

#include <alsa/asoundlib.h>

typedef void (*alsa_mixer_event_handler)(void *userdata);

struct alsa_mixer {

	/* The ALSA mixer handle. */
	snd_mixer_t *mixer;
	snd_mixer_elem_t *elem;

	bool has_db_scale;
	bool has_mute_switch;
	long volume_min_value;
	long volume_max_value;

	alsa_mixer_event_handler event_handler;
	void *event_handler_userdata;

};

void alsa_mixer_init(
		struct alsa_mixer *mixer,
		alsa_mixer_event_handler handler,
		void *userdata);

int alsa_mixer_open(
		struct alsa_mixer *mixer,
		const char *dev_name,
		const char *elem_name,
		unsigned int elem_idx,
		char **msg);

void alsa_mixer_close(
		struct alsa_mixer *mixer);

inline static bool alsa_mixer_is_open(
		const struct alsa_mixer *mixer) {
	return mixer->mixer != NULL && mixer->elem != NULL;
}

inline static int alsa_mixer_poll_descriptors_count(
		struct alsa_mixer *mixer) {
	return snd_mixer_poll_descriptors_count(mixer->mixer);
}

inline static int alsa_mixer_poll_descriptors(
		struct alsa_mixer *mixer,
		struct pollfd* pfds,
		unsigned int space) {
	return snd_mixer_poll_descriptors(mixer->mixer, pfds, space);
}

inline static int alsa_mixer_handle_events(
		struct alsa_mixer *mixer) {
	return snd_mixer_handle_events(mixer->mixer);
}

int alsa_mixer_get_volume(
		const struct alsa_mixer *mixer,
		unsigned int vmax,
		unsigned int *volume,
		bool *muted);

int alsa_mixer_set_volume(
		struct alsa_mixer *mixer,
		unsigned int vmax,
		unsigned int volume,
		bool muted);

#endif
