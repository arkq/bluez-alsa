/*
 * BlueALSA - ba-transport-pcm.h
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_BATRANSPORTPCM_H_
#define BLUEALSA_BATRANSPORTPCM_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include <glib.h>

enum ba_transport_pcm_mode {
	/* PCM used for capturing audio */
	BA_TRANSPORT_PCM_MODE_SOURCE,
	/* PCM used for playing audio */
	BA_TRANSPORT_PCM_MODE_SINK,
};

enum ba_transport_pcm_state {
	BA_TRANSPORT_PCM_STATE_IDLE,
	BA_TRANSPORT_PCM_STATE_STARTING,
	BA_TRANSPORT_PCM_STATE_RUNNING,
	BA_TRANSPORT_PCM_STATE_STOPPING,
	BA_TRANSPORT_PCM_STATE_JOINING,
	BA_TRANSPORT_PCM_STATE_TERMINATED,
};

/**
 * Builder for 16-bit PCM stream format identifier. */
#define BA_TRANSPORT_PCM_FORMAT(sign, width, bytes, endian) \
	(((sign & 1) << 15) | ((endian & 1) << 14) | ((bytes & 0x3F) << 8) | (width & 0xFF))

#define BA_TRANSPORT_PCM_FORMAT_SIGN(format)   (((format) >> 15) & 0x1)
#define BA_TRANSPORT_PCM_FORMAT_WIDTH(format)  ((format) & 0xFF)
#define BA_TRANSPORT_PCM_FORMAT_BYTES(format)  (((format) >> 8) & 0x3F)
#define BA_TRANSPORT_PCM_FORMAT_ENDIAN(format) (((format) >> 14) & 0x1)

#define BA_TRANSPORT_PCM_FORMAT_U8      BA_TRANSPORT_PCM_FORMAT(0, 8, 1, 0)
#define BA_TRANSPORT_PCM_FORMAT_S16_2LE BA_TRANSPORT_PCM_FORMAT(1, 16, 2, 0)
#define BA_TRANSPORT_PCM_FORMAT_S24_3LE BA_TRANSPORT_PCM_FORMAT(1, 24, 3, 0)
#define BA_TRANSPORT_PCM_FORMAT_S24_4LE BA_TRANSPORT_PCM_FORMAT(1, 24, 4, 0)
#define BA_TRANSPORT_PCM_FORMAT_S32_4LE BA_TRANSPORT_PCM_FORMAT(1, 32, 4, 0)

enum ba_transport_pcm_channel {
	BA_TRANSPORT_PCM_CHANNEL_MONO, /* mono */
	BA_TRANSPORT_PCM_CHANNEL_FL,   /* front left */
	BA_TRANSPORT_PCM_CHANNEL_FR,   /* front right */
	BA_TRANSPORT_PCM_CHANNEL_FC,   /* front center */
	BA_TRANSPORT_PCM_CHANNEL_RL,   /* rear left */
	BA_TRANSPORT_PCM_CHANNEL_RR,   /* rear right */
	BA_TRANSPORT_PCM_CHANNEL_SL,   /* side left */
	BA_TRANSPORT_PCM_CHANNEL_SR,   /* side right */
	BA_TRANSPORT_PCM_CHANNEL_LFE,  /* low frequency effects */
};

struct ba_transport_pcm_volume {
	/* volume level change in "dB * 100" */
	int level;
	/* audio signal mute switches */
	bool soft_mute;
	bool hard_mute;
	/* calculated PCM scale factor based on decibel formula
	 * pow(10, dB / 20); for muted channel it shall equal 0 */
	double scale;
};

enum ba_transport_pcm_signal {
	BA_TRANSPORT_PCM_SIGNAL_OPEN,
	BA_TRANSPORT_PCM_SIGNAL_CLOSE,
	BA_TRANSPORT_PCM_SIGNAL_PAUSE,
	BA_TRANSPORT_PCM_SIGNAL_RESUME,
	BA_TRANSPORT_PCM_SIGNAL_DRAIN,
	BA_TRANSPORT_PCM_SIGNAL_DROP,
};

struct ba_transport;

struct ba_transport_pcm {

	/* backward reference to transport */
	struct ba_transport *t;

	/* PCM stream operation mode */
	enum ba_transport_pcm_mode mode;
	/* indicates a master PCM */
	bool master;

	/* guard PCM data updates */
	pthread_mutex_t mutex;
	/* updates notification */
	pthread_cond_t cond;

	pthread_mutex_t state_mtx;
	/* current state of the PCM */
	enum ba_transport_pcm_state state;

	/* PCM file descriptor */
	int fd;
	/* clone of BT socket */
	int fd_bt;

	/* indicates whether PCM is running */
	bool paused;

	/* 16-bit stream format identifier */
	uint16_t format;
	/* number of audio channels */
	unsigned int channels;
	/* PCM sample rate */
	unsigned int rate;

	/* Delay caused by the codec due to internal buffering. The delay is
	 * expressed in 1/10 of millisecond. */
	unsigned int codec_delay_dms;
	/* Delay caused by data processing. This delay component depends on
	 * the host computational power. It is used to compensate for the time
	 * required to encode or decode audio. */
	unsigned int processing_delay_dms;
	/* The last reported total codec + processing delay. It is used to limit
	 * the rate at which changes are reported via D-Bus. */
	unsigned int reported_codec_delay_dms;
	/* Positive (or negative) delay reported by the client. */
	int client_delay_dms;

	/* Indicates whether FIFO buffer was drained. */
	bool drained;

	/* internal software volume control */
	bool soft_volume;

	/* channel map for current PCM configuration */
	enum ba_transport_pcm_channel channel_map[8];

	/* per-channel volume */
	struct ba_transport_pcm_volume volume[8];

	/* new PCM client mutex */
	pthread_mutex_t client_mtx;

	/* source watch for controller socket */
	GSource *controller;

	/* actual thread ID */
	pthread_t tid;

	/* notification PIPE */
	int pipe[2];

	/* exported PCM D-Bus API */
	char *ba_dbus_path;
	bool ba_dbus_exported;

};

int transport_pcm_init(
		struct ba_transport_pcm *pcm,
		enum ba_transport_pcm_mode mode,
		struct ba_transport *t,
		bool master);
void transport_pcm_free(
		struct ba_transport_pcm *pcm);

int ba_transport_pcm_state_set(
		struct ba_transport_pcm *pcm,
		enum ba_transport_pcm_state state);

#define ba_transport_pcm_state_set_idle(pcm) \
	ba_transport_pcm_state_set(pcm, BA_TRANSPORT_PCM_STATE_IDLE)
#define ba_transport_pcm_state_set_running(pcm) \
	ba_transport_pcm_state_set(pcm, BA_TRANSPORT_PCM_STATE_RUNNING)
#define ba_transport_pcm_state_set_stopping(pcm) \
	ba_transport_pcm_state_set(pcm, BA_TRANSPORT_PCM_STATE_STOPPING)

bool ba_transport_pcm_state_check(
		const struct ba_transport_pcm *pcm,
		enum ba_transport_pcm_state state);

#define ba_transport_pcm_state_check_idle(pcm) \
	ba_transport_pcm_state_check(pcm, BA_TRANSPORT_PCM_STATE_IDLE)
#define ba_transport_pcm_state_check_running(pcm) \
	ba_transport_pcm_state_check(pcm, BA_TRANSPORT_PCM_STATE_RUNNING)
#define ba_transport_pcm_state_check_terminated(pcm) \
	ba_transport_pcm_state_check(pcm, BA_TRANSPORT_PCM_STATE_TERMINATED)

int ba_transport_pcm_state_wait(
		const struct ba_transport_pcm *pcm,
		enum ba_transport_pcm_state state);

#define ba_transport_pcm_state_wait_running(pcm) \
	ba_transport_pcm_state_wait(pcm, BA_TRANSPORT_PCM_STATE_RUNNING)
#define ba_transport_pcm_state_wait_terminated(pcm) \
	ba_transport_pcm_state_wait(pcm, BA_TRANSPORT_PCM_STATE_TERMINATED)

/**
 * Transport PCM encoder/decoder IO thread function. */
typedef void *(*ba_transport_pcm_thread_func)(struct ba_transport_pcm *);

#define debug_transport_pcm_thread_loop(pcm, tag) \
	debug("PCM IO loop: %s: %s: %s", tag, __func__, ba_transport_debug_name((pcm)->t))

void ba_transport_pcm_thread_cleanup(struct ba_transport_pcm *pcm);

struct ba_transport_pcm *ba_transport_pcm_ref(struct ba_transport_pcm *pcm);
void ba_transport_pcm_unref(struct ba_transport_pcm *pcm);

int ba_transport_pcm_bt_acquire(struct ba_transport_pcm *pcm);
int ba_transport_pcm_bt_release(struct ba_transport_pcm *pcm);

int ba_transport_pcm_start(
		struct ba_transport_pcm *pcm,
		ba_transport_pcm_thread_func th_func,
		const char *name);
void ba_transport_pcm_stop(
		struct ba_transport_pcm *pcm);

int ba_transport_pcm_release(struct ba_transport_pcm *pcm);

int ba_transport_pcm_pause(struct ba_transport_pcm *pcm);
int ba_transport_pcm_resume(struct ba_transport_pcm *pcm);
int ba_transport_pcm_drain(struct ba_transport_pcm *pcm);
int ba_transport_pcm_drop(struct ba_transport_pcm *pcm);

int ba_transport_pcm_signal_send(
		struct ba_transport_pcm *pcm,
		enum ba_transport_pcm_signal signal);
enum ba_transport_pcm_signal ba_transport_pcm_signal_recv(
		struct ba_transport_pcm *pcm);

bool ba_transport_pcm_is_active(const struct ba_transport_pcm *pcm);

unsigned int ba_transport_pcm_volume_level_to_range(int value, int max);
int ba_transport_pcm_volume_range_to_level(int value, int max);

void ba_transport_pcm_volume_set(
		struct ba_transport_pcm_volume *volume,
		const int *level,
		const bool *soft_mute,
		const bool *hard_mute);

int ba_transport_pcm_volume_sync(
		struct ba_transport_pcm *pcm,
		unsigned int update_mask);

int ba_transport_pcm_get_hardware_volume(
		const struct ba_transport_pcm *pcm);

int ba_transport_pcm_delay_get(
		const struct ba_transport_pcm *pcm);

int ba_transport_pcm_delay_sync(
		struct ba_transport_pcm *pcm,
		unsigned int update_mask);

const char *ba_transport_pcm_channel_to_string(
		enum ba_transport_pcm_channel channel);

#endif
