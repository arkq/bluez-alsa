/*
 * BlueALSA - storage.c
 * Copyright (c) 2016-2023 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "storage.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <bluetooth/bluetooth.h>

#include <glib.h>

#include "ba-transport.h"
#include "hfp.h"
#include "utils.h"
#include "shared/a2dp-codecs.h"
#include "shared/defs.h"
#include "shared/log.h"

#define BA_STORAGE_KEY_DELAY_ADJUSTMENT "DelayAdjustments"
#define BA_STORAGE_KEY_SOFT_VOLUME      "SoftVolume"
#define BA_STORAGE_KEY_VOLUME           "Volume"
#define BA_STORAGE_KEY_MUTE             "Mute"

struct storage {
	/* remote BT device address */
	bdaddr_t addr;
	/* associated storage file */
	GKeyFile *keyfile;
};

static char storage_root_dir[128];
static pthread_mutex_t storage_mutex = PTHREAD_MUTEX_INITIALIZER;
static GHashTable *storage_map = NULL;

static struct storage *storage_lookup(const bdaddr_t *addr) {
	return g_hash_table_lookup(storage_map, addr);
}

static struct storage *storage_new(const bdaddr_t *addr) {

	struct storage *st;
	/* return existing storage if it exists */
	if ((st = storage_lookup(addr)) != NULL)
		goto final;

	if ((st = malloc(sizeof(*st))) == NULL)
		goto final;

	bacpy(&st->addr, addr);
	st->keyfile = g_key_file_new();

	/* Insert a new storage into the map. Please, note that the key is a pointer
	 * to memory stored in the value structure. This is fine as long as the value
	 * is not replaced during key insertion. In such case the old (dangling) key
	 * pointer might/will be used to access the new value! */
	g_hash_table_insert(storage_map, &st->addr, st);

final:
	return st;
}

static void storage_free(struct storage *st) {
	if (st == NULL)
		return;
	g_key_file_free(st->keyfile);
	free(st);
}

/**
 * Initialize BlueALSA persistent storage.
 *
 * @param root The root directory for the persistent storage.
 * @return On success this function returns 0. Otherwise -1 is returned. */
int storage_init(const char *root) {

	debug("Initializing persistent storage: %s", root);
	strncpy(storage_root_dir, root, sizeof(storage_root_dir) - 1);
	if (mkdir(storage_root_dir, S_IRWXU) == -1 && errno != EEXIST)
		warn("Couldn't create storage directory: %s", strerror(errno));

	if (storage_map == NULL)
		storage_map = g_hash_table_new_full(g_bdaddr_hash, g_bdaddr_equal,
				NULL, (GDestroyNotify)storage_free);

	return 0;
}

/**
 * Cleanup resources allocated by the persistent storage. */
void storage_destroy(void) {
	if (storage_map == NULL)
		return;
	g_hash_table_unref(storage_map);
	storage_map = NULL;
}

/**
 * Load persistent storage file for the given BT device. */
int storage_device_load(const struct ba_device *d) {

	char addrstr[18];
	char path[sizeof(storage_root_dir) + sizeof(addrstr)];
	ba2str(&d->addr, addrstr);
	snprintf(path, sizeof(path), "%s/%s", storage_root_dir, addrstr);
	int rv = -1;

	pthread_mutex_lock(&storage_mutex);

	debug("Loading storage: %s", path);

	struct storage *st;
	if ((st = storage_new(&d->addr)) == NULL)
		goto final;

	GError *err = NULL;
	if (!g_key_file_load_from_file(st->keyfile, path, G_KEY_FILE_NONE, &err)) {
		if (err->code != G_FILE_ERROR_NOENT)
			warn("Couldn't load storage: %s", err->message);
		g_error_free(err);
		goto final;
	}

	rv = 0;

final:
	pthread_mutex_unlock(&storage_mutex);
	return rv;
}

/**
 * Save persistent storage file for the given BT device. */
int storage_device_save(const struct ba_device *d) {

	char addrstr[18];
	char path[sizeof(storage_root_dir) + sizeof(addrstr)];
	ba2str(&d->addr, addrstr);
	snprintf(path, sizeof(path), "%s/%s", storage_root_dir, addrstr);
	int rv = -1;

	pthread_mutex_lock(&storage_mutex);

	struct storage *st;
	if ((st = storage_lookup(&d->addr)) == NULL)
		goto final;

	debug("Saving storage: %s", path);

	GError *err = NULL;
	if (!g_key_file_save_to_file(st->keyfile, path, &err)) {
		error("Couldn't save storage: %s", err->message);
		g_error_free(err);
		goto final;
	}

	/* remove the storage from the map */
	g_hash_table_remove(storage_map, &d->addr);

	rv = 0;

final:
	pthread_mutex_unlock(&storage_mutex);
	return rv;
}

/**
 * Synchronize PCM with persistent storage.
 *
 * @param pcm The PCM structure to synchronize.
 * @return This function returns 1 or 0 respectively if the storage data was
 *   synchronized or not. On error -1 is returned. */
int storage_pcm_data_sync(struct ba_transport_pcm *pcm) {

	const struct ba_transport *t = pcm->t;
	const struct ba_device *d = t->d;
	int rv = 0;

	pthread_mutex_lock(&storage_mutex);

	struct storage *st;
	if ((st = storage_lookup(&d->addr)) == NULL)
		goto final;

	GKeyFile *keyfile = st->keyfile;
	const char *group = pcm->ba_dbus_path;

	if (!g_key_file_has_group(keyfile, group))
		goto final;

	if (g_key_file_has_key(keyfile, group, BA_STORAGE_KEY_DELAY_ADJUSTMENT, NULL)) {
		gsize length;
		char **adjustments = g_key_file_get_string_list(keyfile, group,
				BA_STORAGE_KEY_DELAY_ADJUSTMENT, &length, NULL);
		for (gsize index = 0; index < length; index++) {
			char *codec_name = adjustments[index];
			char *value = strchr(adjustments[index], ':');
			if (value == NULL)
				continue;
			*value++ = '\0';
			uint16_t codec_id = 0xFFFF;
			if (t->profile & BA_TRANSPORT_PROFILE_MASK_A2DP &&
					(codec_id = a2dp_codecs_codec_id_from_string(codec_name)) == 0xFFFF)
				continue;
			if (t->profile & BA_TRANSPORT_PROFILE_MASK_SCO &&
					(codec_id = hfp_codec_id_from_string(codec_name)) == HFP_CODEC_UNDEFINED)
				continue;
			int16_t adjustment = atoi(value);
			ba_transport_pcm_delay_adjustment_set(pcm, codec_id, adjustment);
		}
		g_strfreev(adjustments);
		rv = 1;
	}

	if (g_key_file_has_key(keyfile, group, BA_STORAGE_KEY_SOFT_VOLUME, NULL)) {
		pcm->soft_volume = g_key_file_get_boolean(keyfile, group,
				BA_STORAGE_KEY_SOFT_VOLUME, NULL);
		rv = 1;
	}

	if (g_key_file_has_key(keyfile, group, BA_STORAGE_KEY_VOLUME, NULL)) {
		int *list;
		gsize len = 0;
		if ((list = g_key_file_get_integer_list(keyfile, group,
						BA_STORAGE_KEY_VOLUME, &len, NULL)) != NULL &&
				len == 2) {
			ba_transport_pcm_volume_set(&pcm->volume[0], &list[0], NULL, NULL);
			ba_transport_pcm_volume_set(&pcm->volume[1], &list[1], NULL, NULL);
		}
		g_free(list);
		rv = 1;
	}

	if (g_key_file_has_key(keyfile, group, BA_STORAGE_KEY_MUTE, NULL)) {
		gboolean *list;
		gsize len = 0;
		if ((list = g_key_file_get_boolean_list(keyfile, group,
						BA_STORAGE_KEY_MUTE, &len, NULL)) != NULL &&
				len == 2) {
			const bool mute[2] = { list[0], list[1] };
			ba_transport_pcm_volume_set(&pcm->volume[0], NULL, &mute[0], NULL);
			ba_transport_pcm_volume_set(&pcm->volume[1], NULL, &mute[1], NULL);
		}
		g_free(list);
		rv = 1;
	}

final:
	pthread_mutex_unlock(&storage_mutex);
	return rv;
}

/**
 * Update persistent storage with PCM data.
 *
 * @param pcm The PCM structure for which to update the storage.
 * @return On success this function returns 0. Otherwise -1 is returned. */
int storage_pcm_data_update(const struct ba_transport_pcm *pcm) {

	const struct ba_transport *t = pcm->t;
	const struct ba_device *d = t->d;
	int rv = -1;

	pthread_mutex_lock(&storage_mutex);

	struct storage *st;
	if ((st = storage_lookup(&d->addr)) == NULL)
		if ((st = storage_new(&d->addr)) == NULL)
			goto final;

	GKeyFile *keyfile = st->keyfile;
	const char *group = pcm->ba_dbus_path;

	const size_t num_codecs = g_hash_table_size(pcm->delay_adjustments);
	char **list = calloc(num_codecs + 1, sizeof(char *));

	pthread_mutex_lock(MUTABLE(&pcm->delay_adjustments_mtx));

	GHashTableIter iter;
	g_hash_table_iter_init(&iter, pcm->delay_adjustments);

	size_t index = 0;
	void *key, *value;
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		const char *codec = NULL;
		if (t->profile & BA_TRANSPORT_PROFILE_MASK_A2DP)
			codec = a2dp_codecs_codec_id_to_string(GPOINTER_TO_INT(key));
		if (t->profile & BA_TRANSPORT_PROFILE_MASK_SCO)
			codec = hfp_codec_id_to_string(GPOINTER_TO_INT(key));
		if (codec == NULL)
			continue;
		list[index] = malloc(strlen(codec) + 1 + 6 + 1);
		sprintf(list[index], "%s:%d", codec, GPOINTER_TO_INT(value));
		index++;
	}

	pthread_mutex_unlock(MUTABLE(&pcm->delay_adjustments_mtx));

	g_key_file_set_string_list(keyfile, group, BA_STORAGE_KEY_DELAY_ADJUSTMENT,
		(const char * const *)list, num_codecs);

	for (index = 0; index < num_codecs; index++)
		free(list[index]);
	free(list);

	g_key_file_set_boolean(keyfile, group, BA_STORAGE_KEY_SOFT_VOLUME,
			pcm->soft_volume);

	int volume[2] = { pcm->volume[0].level, pcm->volume[1].level };
	g_key_file_set_integer_list(keyfile, group, BA_STORAGE_KEY_VOLUME, volume, 2);

	gboolean mute[2] = { pcm->volume[0].soft_mute, pcm->volume[1].soft_mute };
	g_key_file_set_boolean_list(keyfile, group, BA_STORAGE_KEY_MUTE, mute, 2);

	rv = 0;

final:
	pthread_mutex_unlock(&storage_mutex);
	return rv;
}
