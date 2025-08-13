/*
 * btd.inc
 * vim: ft=c
 *
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <ctype.h>
#include <endian.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "a2dp.h"
#include "ba-transport.h"
#include "hfp.h"
#include "io.h"
#include "utils.h"
#include "shared/a2dp-codecs.h"
#include "shared/defs.h"
#include "shared/ffb.h"
#include "shared/log.h"

#define BT_DUMP_CURRENT_VERSION 2

/**
 * BT dump mode. */
enum bt_dump_mode {
	BT_DUMP_MODE_A2DP_SOURCE = 1,
	BT_DUMP_MODE_A2DP_SINK,
	BT_DUMP_MODE_SCO,
};

/**
 * BT dump handle. */
struct bt_dump {
	unsigned int version;
	enum bt_dump_mode mode;
	uint32_t transport_codec_id;
	a2dp_t a2dp_configuration;
	size_t a2dp_configuration_size;
	FILE *file;
};

/**
 * Read a single data record from a BT dump file. */
ssize_t bt_dump_read(struct bt_dump *btd, void *data, size_t size) {
	uint16_t len;
	if (fread(&len, sizeof(len), 1, btd->file) != 1)
		return -1;
	size = MIN(be16toh(len), size);
	if (fread(data, 1, size, btd->file) != size)
		return -1;
	return size;
}

/**
 * Write a single data record to a BT dump file. */
ssize_t bt_dump_write(struct bt_dump *btd, const void *data, size_t size) {
	uint16_t len = htobe16(size);
	if (fwrite(&len, sizeof(len), 1, btd->file) != 1)
		return -1;
	if (fwrite(data, 1, size, btd->file) != size)
		return -1;
	fflush(btd->file);
	return sizeof(len) + size;
}

/**
 * Close BT dump file. */
void bt_dump_close(struct bt_dump *btd) {
	if (btd == NULL)
		return;
	if (btd->file != NULL)
		fclose(btd->file);
	free(btd);
}

/**
 * Create BT dump file. */
struct bt_dump *bt_dump_create(
		const char *path,
		struct ba_transport *t) {

	struct bt_dump *btd;
	if ((btd = calloc(1, sizeof(*btd))) == NULL)
		return NULL;

	if ((btd->file = fopen(path, "wb")) == NULL)
		goto fail;

	enum bt_dump_mode mode = 0;
	const void *a2dp_configuration = NULL;
	size_t a2dp_configuration_size = 0;

	switch (t->profile) {
	case BA_TRANSPORT_PROFILE_NONE:
#if ENABLE_MIDI
	case BA_TRANSPORT_PROFILE_MIDI:
#endif
		break;
	case BA_TRANSPORT_PROFILE_A2DP_SOURCE:
		a2dp_configuration = &t->media.configuration;
		a2dp_configuration_size = t->media.sep->config.caps_size;
		mode = BT_DUMP_MODE_A2DP_SOURCE;
		break;
	case BA_TRANSPORT_PROFILE_A2DP_SINK:
		a2dp_configuration = &t->media.configuration;
		a2dp_configuration_size = t->media.sep->config.caps_size;
		mode = BT_DUMP_MODE_A2DP_SINK;
		break;
	case BA_TRANSPORT_PROFILE_HFP_AG:
	case BA_TRANSPORT_PROFILE_HFP_HF:
	case BA_TRANSPORT_PROFILE_HSP_AG:
	case BA_TRANSPORT_PROFILE_HSP_HS:
		mode = BT_DUMP_MODE_SCO;
		break;
	}

	unsigned int mode_ = mode;
	fprintf(btd->file, "BA.dump-%1x.%1x.", BT_DUMP_CURRENT_VERSION, mode_);

	btd->transport_codec_id = ba_transport_get_codec(t);
	uint32_t id = htobe32(btd->transport_codec_id);
	if (bt_dump_write(btd, &id, sizeof(id)) == -1)
		goto fail;

	if (a2dp_configuration != NULL) {
		memcpy(&btd->a2dp_configuration, a2dp_configuration,
				MIN(a2dp_configuration_size, sizeof(btd->a2dp_configuration)));
		btd->a2dp_configuration_size = a2dp_configuration_size;
		if (bt_dump_write(btd, a2dp_configuration, a2dp_configuration_size) == -1)
			goto fail;
	}

	return btd;

fail:
	bt_dump_close(btd);
	return NULL;
}

/**
 * Open BT dump file. */
struct bt_dump *bt_dump_open(const char *path) {

	struct bt_dump *btd;
	if ((btd = calloc(1, sizeof(*btd))) == NULL)
		return NULL;

	if ((btd->file = fopen(path, "rb")) == NULL)
		goto fail;

	unsigned int mode;
	if (fscanf(btd->file, "BA.dump-%1x.%1x.", &btd->version, &mode) != 2) {
		errno = EIO;
		goto fail;
	}

	if (btd->version > BT_DUMP_CURRENT_VERSION) {
		errno = EINVAL;
		goto fail;
	}

	uint32_t id;
	if (bt_dump_read(btd, &id, sizeof(id)) == -1)
		goto fail;
	btd->transport_codec_id = be32toh(id);

	switch (btd->mode = mode) {
	case BT_DUMP_MODE_A2DP_SOURCE:
	case BT_DUMP_MODE_A2DP_SINK: {
		ssize_t size = sizeof(btd->a2dp_configuration);
		if ((size = bt_dump_read(btd, &btd->a2dp_configuration, size)) == -1)
			goto fail;
		btd->a2dp_configuration_size = size;
	} break;
	case BT_DUMP_MODE_SCO:
		break;
	default:
		errno = EINVAL;
		goto fail;
	}

	return btd;

fail:
	bt_dump_close(btd);
	return NULL;
}

static const char *transport_to_fname(const struct ba_transport *t) {

	const uint32_t codec_id = ba_transport_get_codec(t);
	const char *profile = NULL;
	const char *codec = NULL;

	switch (t->profile) {
	case BA_TRANSPORT_PROFILE_NONE:
		return "none";
	case BA_TRANSPORT_PROFILE_A2DP_SOURCE:
		profile = "a2dp-source";
		codec = a2dp_codecs_codec_id_to_string(codec_id);
		break;
	case BA_TRANSPORT_PROFILE_A2DP_SINK:
		profile = "a2dp-sink";
		codec = a2dp_codecs_codec_id_to_string(codec_id);
		break;
	case BA_TRANSPORT_PROFILE_HFP_AG:
		profile = "hfp-ag";
		codec = hfp_codec_id_to_string(codec_id);
		break;
	case BA_TRANSPORT_PROFILE_HFP_HF:
		profile = "hfp-hf";
		codec = hfp_codec_id_to_string(codec_id);
		break;
	case BA_TRANSPORT_PROFILE_HSP_AG:
		profile = "hsp-ag";
		codec = hfp_codec_id_to_string(codec_id);
		break;
	case BA_TRANSPORT_PROFILE_HSP_HS:
		profile = "hsp-hs";
		codec = hfp_codec_id_to_string(codec_id);
		break;
#if ENABLE_MIDI
	case BA_TRANSPORT_PROFILE_MIDI:
		return "midi";
#endif
	}

	char fallback[16];
	if (codec == NULL) {
		snprintf(fallback, sizeof(fallback), "%08x", codec_id);
		codec = fallback;
	}

	static char buffer[64];
	snprintf(buffer, sizeof(buffer), "%s-%s", profile, codec);
	return buffer;
}

static const char *transport_pcm_to_fname(const struct ba_transport_pcm *t_pcm) {
	static char buffer[64];
	snprintf(buffer, sizeof(buffer), "s%u-%u-%uc",
			BA_TRANSPORT_PCM_FORMAT_WIDTH(t_pcm->format), t_pcm->rate, t_pcm->channels);
	return buffer;
}

/**
 * Dump incoming BT data to a BT dump file. */
void *bt_dump_io_thread(struct ba_transport_pcm *t_pcm) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pcm_thread_cleanup), t_pcm);

	struct ba_transport *t = t_pcm->t;
	struct io_poll io = { .timeout = -1 };
	ffb_t bt = { 0 };

	struct bt_dump *btd = NULL;
	char fname[256];

	snprintf(fname, sizeof(fname), "/tmp/bluealsa-%s-%s.btd",
			transport_to_fname(t), transport_pcm_to_fname(t_pcm));

	debug("Creating BT dump file: %s", fname);
	if ((btd = bt_dump_create(fname, t)) == NULL) {
		error("Couldn't create BT dump file: %s", strerror(errno));
		goto fail_open;
	}

	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &bt);
	pthread_cleanup_push(PTHREAD_CLEANUP(bt_dump_close), btd);

	if (ffb_init_uint8_t(&bt, t->mtu_read) == -1) {
		error("Couldn't create data buffer: %s", strerror(errno));
		goto fail_ffb;
	}

	debug_transport_pcm_thread_loop(t_pcm, "START");
	for (ba_transport_pcm_state_set_running(t_pcm);;) {

		ssize_t len;
		ffb_rewind(&bt);
		if ((len = io_poll_and_read_bt(&io, t_pcm, &bt)) <= 0) {
			if (len == -1)
				error("BT poll and read error: %s", strerror(errno));
			goto fail;
		}

		debug("BT read: %zd", len);
		bt_dump_write(btd, bt.data, len);

	}

fail:
	debug_transport_pcm_thread_loop(t_pcm, "EXIT");
fail_ffb:
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
fail_open:
	pthread_cleanup_pop(1);
	return NULL;
}
