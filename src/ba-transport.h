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

#include "ba-device.h"
#include "hfp.h"

#define BA_TRANSPORT_PROFILE_NONE        (0)
#define BA_TRANSPORT_PROFILE_A2DP_SOURCE (1 << 0)
#define BA_TRANSPORT_PROFILE_A2DP_SINK   (2 << 0)
#define BA_TRANSPORT_PROFILE_HFP_HF      (1 << 2)
#define BA_TRANSPORT_PROFILE_HFP_AG      (2 << 2)
#define BA_TRANSPORT_PROFILE_HSP_HS      (1 << 4)
#define BA_TRANSPORT_PROFILE_HSP_AG      (2 << 4)
#define BA_TRANSPORT_PROFILE_RFCOMM      (1 << 6)

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

#define IS_BA_TRANSPORT_PROFILE_SCO(p) \
	((p) && ((p) & BA_TRANSPORT_PROFILE_MASK_SCO) == (p))

/**
 * Selected profile and audio codec.
 *
 * For A2DP vendor codecs the upper byte of the codec field
 * contains the lowest byte of the vendor ID. */
struct ba_transport_type {
	uint16_t profile;
	uint16_t codec;
};

enum ba_transport_state {
	BA_TRANSPORT_STATE_IDLE,
	BA_TRANSPORT_STATE_PENDING,
	BA_TRANSPORT_STATE_ACTIVE,
	BA_TRANSPORT_STATE_PAUSED,
};

enum ba_transport_signal {
	BA_TRANSPORT_SIGNAL_PING,
	BA_TRANSPORT_SIGNAL_PCM_OPEN,
	BA_TRANSPORT_SIGNAL_PCM_CLOSE,
	BA_TRANSPORT_SIGNAL_PCM_PAUSE,
	BA_TRANSPORT_SIGNAL_PCM_RESUME,
	BA_TRANSPORT_SIGNAL_PCM_SYNC,
	BA_TRANSPORT_SIGNAL_PCM_DROP,
	BA_TRANSPORT_SIGNAL_HFP_SET_CODEC_CVSD,
	BA_TRANSPORT_SIGNAL_HFP_SET_CODEC_MSBC,
	BA_TRANSPORT_SIGNAL_UPDATE_BATTERY,
	BA_TRANSPORT_SIGNAL_UPDATE_VOLUME,
};

enum ba_transport_pcm_mode {
	/* PCM used for capturing audio */
	BA_TRANSPORT_PCM_MODE_SOURCE,
	/* PCM used for playing audio */
	BA_TRANSPORT_PCM_MODE_SINK,
};

/**
 * Builder for 16-bit PCM stream format identifier. */
#define BA_TRANSPORT_PCM_FORMAT(sign, width, byteorder) \
	(((sign & 1) << 15) | ((byteorder & 1) << 14) | ((width) & 0x3F))

#define BA_TRANSPORT_PCM_FORMAT_U8    BA_TRANSPORT_PCM_FORMAT(0, 8, 0)
#define BA_TRANSPORT_PCM_FORMAT_S16LE BA_TRANSPORT_PCM_FORMAT(1, 16, 0)
#define BA_TRANSPORT_PCM_FORMAT_S24LE BA_TRANSPORT_PCM_FORMAT(1, 24, 0)

struct ba_transport_pcm {

	/* backward reference to transport */
	struct ba_transport *t;

	/* FIFO file descriptor */
	int fd;
	/* associated client */
	int client;

	/* PCM stream operation mode */
	enum ba_transport_pcm_mode mode;

	/* 16-bit stream format identifier */
	uint16_t format;
	/* number of audio channels */
	unsigned int channels;
	/* PCM sampling frequency */
	unsigned int sampling;

	/* Overall PCM delay in 1/10 of millisecond, caused by
	 * audio encoding or decoding and data transfer. */
	unsigned int delay;

	/* Volume configuration for channel left [0] and right [1]. In case of
	 * a monophonic sound, left [0] channel shall be used. Also note, that
	 * A2DP and SCO profiles use different volume level ranges:
	 * A2DP - [0, 127], SCO - [0, 15]. */
	struct {
		unsigned int level;
		bool muted;
	} volume[2];

	/* exported PCM D-Bus API */
	char *ba_dbus_path;
	unsigned int ba_dbus_id;

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

	/* IO thread - actual transport layer */
	enum ba_transport_state state;
	pthread_t thread;

	/* This field stores a file descriptor (socket) associated with the BlueZ
	 * side of the transport. The role of this socket depends on the transport
	 * type - it can be either A2DP, RFCOMM or SCO link. */
	int bt_fd;

	/* max transfer unit values for bt_fd */
	size_t mtu_read;
	size_t mtu_write;

	/* PIPE used to notify thread about changes. If thread is based on loop with
	 * an event wait syscall (e.g. poll), this file descriptor is used to send a
	 * control event. */
	int sig_fd[2];

	union {

		struct {

			/* delay reported by the AVDTP */
			uint16_t delay;

			struct ba_transport_pcm pcm;

			/* selected audio codec configuration */
			uint8_t *cconfig;
			size_t cconfig_size;

			/* Value reported by the ioctl(TIOCOUTQ) when the output buffer is
			 * empty. Somehow this ioctl call reports "available" buffer space.
			 * So, in order to get the number of bytes in the queue buffer, we
			 * have to subtract the initial value from values returned by
			 * subsequent ioctl() calls. */
			int bt_fd_coutq_init;

			/* playback synchronization */
			pthread_mutex_t drained_mtx;
			pthread_cond_t drained;

		} a2dp;

		struct {

			/* associated SCO transport */
			struct ba_transport *sco;

			/* AG/HF supported features bitmask */
			uint32_t hfp_features;
			/* received AG indicator values */
			unsigned char hfp_inds[__HFP_IND_MAX];
			/* indicator activation state */
			bool hfp_inds_state[__HFP_IND_MAX];

			/* codec selection synchronization */
			pthread_mutex_t codec_selection_completed_mtx;
			pthread_cond_t codec_selection_completed;

			/* exported RFCOMM D-Bus API */
			char *ba_dbus_path;
			unsigned int ba_dbus_id;

			/* external RFCOMM handler */
			int handler_fd;

		} rfcomm;

		struct {

			/* if true, SCO is handled by oFono */
			bool ofono;

			/* parent RFCOMM transport */
			struct ba_transport *rfcomm;

			/* Speaker and microphone signals should to be exposed as
			 * a separate PCM devices. Hence, there is a requirement
			 * for separate configurations. */
			struct ba_transport_pcm spk_pcm;
			struct ba_transport_pcm mic_pcm;

			/* playback synchronization */
			pthread_mutex_t spk_drained_mtx;
			pthread_cond_t spk_drained;

		} sco;

	};

	/* indicates cleanup lock */
	bool cleanup_lock;

	/* callback functions for self-management */
	int (*acquire)(struct ba_transport *);
	int (*release)(struct ba_transport *);

	/* memory self-management */
	int ref_count;

};

struct ba_transport *ba_transport_new(
		struct ba_device *device,
		const char *dbus_owner,
		const char *dbus_path);
struct ba_transport *ba_transport_new_a2dp(
		struct ba_device *device,
		struct ba_transport_type type,
		const char *dbus_owner,
		const char *dbus_path,
		const void *cconfig,
		size_t cconfig_size);
struct ba_transport *ba_transport_new_rfcomm(
		struct ba_device *device,
		struct ba_transport_type type,
		const char *dbus_owner,
		const char *dbus_path);
struct ba_transport *ba_transport_new_sco(
		struct ba_device *device,
		struct ba_transport_type type,
		const char *dbus_owner,
		const char *dbus_path,
		struct ba_transport *rfcomm);

struct ba_transport *ba_transport_lookup(
		struct ba_device *device,
		const char *dbus_path);
struct ba_transport *ba_transport_ref(
		struct ba_transport *t);

void ba_transport_destroy(struct ba_transport *t);
void ba_transport_unref(struct ba_transport *t);

int ba_transport_send_signal(struct ba_transport *t, enum ba_transport_signal sig);
enum ba_transport_signal ba_transport_recv_signal(struct ba_transport *t);

int ba_transport_select_codec(
		struct ba_transport *t,
		uint16_t codec);
void ba_transport_update_codec(
		struct ba_transport *t,
		uint16_t codec);

uint16_t ba_transport_get_delay(const struct ba_transport *t);

uint16_t ba_transport_pcm_get_volume_packed(const struct ba_transport_pcm *pcm);
int ba_transport_pcm_set_volume_packed(struct ba_transport_pcm *pcm, uint16_t value);

int ba_transport_set_state(struct ba_transport *t, enum ba_transport_state state);

int ba_transport_drain_pcm(struct ba_transport *t);
int ba_transport_release_pcm(struct ba_transport_pcm *pcm);

int ba_transport_pthread_create(
		struct ba_transport *t,
		void *(*routine)(struct ba_transport *),
		const char *name);

void ba_transport_pthread_cleanup(struct ba_transport *t);
int ba_transport_pthread_cleanup_lock(struct ba_transport *t);
int ba_transport_pthread_cleanup_unlock(struct ba_transport *t);

#endif
