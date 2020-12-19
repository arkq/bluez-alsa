/*
 * BlueALSA - ba-transport.h
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_BATRANSPORT_H_
#define BLUEALSA_BATRANSPORT_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "a2dp.h"
#include "ba-device.h"
#include "ba-rfcomm.h"
#include "bluez.h"
#include "bluealsa-pcm-multi.h"

#define BA_TRANSPORT_PROFILE_NONE        (0)
#define BA_TRANSPORT_PROFILE_A2DP_SOURCE (1 << 0)
#define BA_TRANSPORT_PROFILE_A2DP_SINK   (2 << 0)
#define BA_TRANSPORT_PROFILE_HFP_HF      (1 << 2)
#define BA_TRANSPORT_PROFILE_HFP_AG      (2 << 2)
#define BA_TRANSPORT_PROFILE_HSP_HS      (1 << 4)
#define BA_TRANSPORT_PROFILE_HSP_AG      (2 << 4)

#define BA_TRANSPORT_PROFILE_MASK_A2DP \
	(BA_TRANSPORT_PROFILE_A2DP_SOURCE | BA_TRANSPORT_PROFILE_A2DP_SINK)
#define BA_TRANSPORT_PROFILE_MASK_HFP \
	(BA_TRANSPORT_PROFILE_HFP_HF | BA_TRANSPORT_PROFILE_HFP_AG)
#define BA_TRANSPORT_PROFILE_MASK_HSP \
	(BA_TRANSPORT_PROFILE_HSP_HS | BA_TRANSPORT_PROFILE_HSP_AG)
#define BA_TRANSPORT_PROFILE_MASK_SCO \
	(BA_TRANSPORT_PROFILE_MASK_HFP | BA_TRANSPORT_PROFILE_MASK_HSP)
#define BA_TRANSPORT_PROFILE_MASK_AG \
	(BA_TRANSPORT_PROFILE_HSP_AG | BA_TRANSPORT_PROFILE_HFP_AG)
#define BA_TRANSPORT_PROFILE_MASK_HF \
	(BA_TRANSPORT_PROFILE_HSP_HS | BA_TRANSPORT_PROFILE_HFP_HF)

/**
 * Selected profile and audio codec.
 *
 * For A2DP vendor codecs the upper byte of the codec field
 * contains the lowest byte of the vendor ID. */
struct ba_transport_type {
	uint16_t profile;
	uint16_t codec;
};

enum ba_transport_signal {
	BA_TRANSPORT_SIGNAL_PING,
	BA_TRANSPORT_SIGNAL_PCM_OPEN,
	BA_TRANSPORT_SIGNAL_PCM_CLOSE,
	BA_TRANSPORT_SIGNAL_PCM_PAUSE,
	BA_TRANSPORT_SIGNAL_PCM_RESUME,
	BA_TRANSPORT_SIGNAL_PCM_SYNC,
	BA_TRANSPORT_SIGNAL_PCM_DROP,
};

enum ba_transport_pcm_mode {
	/* PCM used for capturing audio */
	BA_TRANSPORT_PCM_MODE_SOURCE,
	/* PCM used for playing audio */
	BA_TRANSPORT_PCM_MODE_SINK,
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

struct ba_transport_pcm {

	/* backward reference to transport */
	struct ba_transport *t;
	/* associated transport thread */
	struct ba_transport_thread *th;

	/* PCM stream operation mode */
	enum ba_transport_pcm_mode mode;

	/* FIFO file descriptor */
	int fd;

	/* Multi-client stream support */
	struct bluealsa_pcm_multi *multi;

	/* 16-bit stream format identifier */
	uint16_t format;
	/* number of audio channels */
	unsigned int channels;
	/* PCM sampling frequency */
	unsigned int sampling;

	/* Overall PCM delay in 1/10 of millisecond, caused by
	 * audio encoding or decoding and data transfer. */
	unsigned int delay;

	/* internal software volume control */
	bool soft_volume;

	/* maximal possible Bluetooth volume */
	unsigned int max_bt_volume;

	/* Volume configuration for channel left [0] and right [1]. In case of
	 * a monophonic sound, only the left [0] channel shall be used. */
	struct {
		/* volume level change in "dB * 100" */
		int level;
		/* audio signal mute switch */
		bool muted;
	} volume[2];

	/* data synchronization */
	pthread_mutex_t synced_mtx;
	pthread_cond_t synced;

	/* exported PCM D-Bus API */
	char *ba_dbus_path;
	unsigned int ba_dbus_id;

};

struct ba_transport_thread {
	/* backward reference to transport */
	struct ba_transport *t;
	/* guard PCM running on this thread */
	pthread_mutex_t mutex;
	/* actual thread ID */
	pthread_t id;
	/* notification PIPE */
	int pipe[2];
	/* indicates cleanup lock */
	bool cleanup_lock;
};

struct ba_transport {

	/* backward reference to device */
	struct ba_device *d;

	/* Transport structure covers all transports supported by BlueALSA. However,
	 * every transport requires specific handling - link acquisition, transport
	 * specific configuration, freeing resources, etc. */
	struct ba_transport_type type;

	/* data for D-Bus management */
	char *bluez_dbus_owner;
	char *bluez_dbus_path;

	/* This mutex shall guard modifications of the critical sections in this
	 * transport structure, e.g. thread creation/termination. */
	pthread_mutex_t mutex;

	/* This field stores a file descriptor (socket) associated with the BlueZ
	 * side of the transport. The role of this socket depends on the transport
	 * type - it can be either A2DP or SCO link. */
	int bt_fd;

	/* max transfer unit values for bt_fd */
	size_t mtu_read;
	size_t mtu_write;

	/* main thread for audio processing */
	struct ba_transport_thread thread;
	/* thread for back-channel processing */
	struct ba_transport_thread thread_bc;

	union {

		struct {

			/* used D-Bus endpoint path */
			const char *bluez_dbus_sep_path;

			/* current state of the transport */
			enum bluez_a2dp_transport_state state;

			/* audio codec configuration capabilities */
			const struct a2dp_codec *codec;
			/* selected audio codec configuration */
			uint8_t *configuration;

			/* delay reported by the AVDTP */
			uint16_t delay;

			struct ba_transport_pcm pcm;
			/* PCM for back-channel stream */
			struct ba_transport_pcm pcm_bc;

			/* Value reported by the ioctl(TIOCOUTQ) when the output buffer is
			 * empty. Somehow this ioctl call reports "available" buffer space.
			 * So, in order to get the number of bytes in the queue buffer, we
			 * have to subtract the initial value from values returned by
			 * subsequent ioctl() calls. */
			int bt_fd_coutq_init;

		} a2dp;

		struct {

			/* associated RFCOMM thread */
			struct ba_rfcomm *rfcomm;

			/* Speaker and microphone signals should to be exposed as
			 * a separate PCM devices. Hence, there is a requirement
			 * for separate configurations. */
			struct ba_transport_pcm spk_pcm;
			struct ba_transport_pcm mic_pcm;

		} sco;

	};

	/* callback functions for self-management */
	int (*acquire)(struct ba_transport *);
	int (*release)(struct ba_transport *);

	/* memory self-management */
	int ref_count;

};

struct ba_transport *ba_transport_new_a2dp(
		struct ba_device *device,
		struct ba_transport_type type,
		const char *dbus_owner,
		const char *dbus_path,
		const struct a2dp_codec *codec,
		const void *configuration);
struct ba_transport *ba_transport_new_sco(
		struct ba_device *device,
		struct ba_transport_type type,
		const char *dbus_owner,
		const char *dbus_path,
		int rfcomm_fd);

struct ba_transport *ba_transport_lookup(
		struct ba_device *device,
		const char *dbus_path);
struct ba_transport *ba_transport_ref(
		struct ba_transport *t);

void ba_transport_destroy(struct ba_transport *t);
void ba_transport_unref(struct ba_transport *t);

struct ba_transport_pcm *ba_transport_pcm_ref(struct ba_transport_pcm *pcm);
void ba_transport_pcm_unref(struct ba_transport_pcm *pcm);

int ba_transport_select_codec_a2dp(
		struct ba_transport *t,
		const struct a2dp_sep *sep);
int ba_transport_select_codec_sco(
		struct ba_transport *t,
		uint16_t codec_id);

void ba_transport_set_codec(
		struct ba_transport *t,
		uint16_t codec_id);

int ba_transport_start(struct ba_transport *t);
int ba_transport_stop(struct ba_transport *t);

int ba_transport_set_a2dp_state(
		struct ba_transport *t,
		enum bluez_a2dp_transport_state state);

int ba_transport_pcm_get_delay(
		const struct ba_transport_pcm *pcm);

unsigned int ba_transport_pcm_volume_level_to_bt(
		const struct ba_transport_pcm *pcm,
		int value);
int ba_transport_pcm_volume_bt_to_level(
		const struct ba_transport_pcm *pcm,
		unsigned int value);

int ba_transport_pcm_volume_update(
		struct ba_transport_pcm *pcm);

int ba_transport_pcm_pause(struct ba_transport_pcm *pcm);
int ba_transport_pcm_resume(struct ba_transport_pcm *pcm);
int ba_transport_pcm_drain(struct ba_transport_pcm *pcm);
int ba_transport_pcm_drop(struct ba_transport_pcm *pcm);

int ba_transport_pcm_release(struct ba_transport_pcm *pcm);

int ba_transport_thread_create(
		struct ba_transport_thread *th,
		void *(*routine)(struct ba_transport_thread *),
		const char *name);

int ba_transport_thread_send_signal(
		struct ba_transport_thread *th,
		enum ba_transport_signal sig);
enum ba_transport_signal ba_transport_thread_recv_signal(
		struct ba_transport_thread *th);

void ba_transport_thread_cleanup(struct ba_transport_thread *th);
int ba_transport_thread_cleanup_lock(struct ba_transport_thread *th);
int ba_transport_thread_cleanup_unlock(struct ba_transport_thread *th);

#endif
