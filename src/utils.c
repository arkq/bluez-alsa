/*
 * BlueALSA - utils.c
 * SPDX-FileCopyrightText: 2016-2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#include "utils.h"

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <bluetooth/bluetooth.h>

#include <glib.h>
#include <glib-object.h>

#include "shared/defs.h"
#include "shared/log.h"

/**
 * Convenience function to free a list of glib objects.
 *
 * @param list A pointer to the GObjectList. */
void g_object_list_free(GObjectList * list) {
	g_list_free_full((GList *)list, g_object_unref);
}

/**
 * Extract HCI device ID from the BlueZ D-Bus object path.
 *
 * @param path BlueZ D-Bus object path.
 * @return On success this function returns ID of the HCI device.
 *   Otherwise, -1 is returned. */
int g_dbus_bluez_object_path_to_hci_dev_id(const char *path) {
	if ((path = strstr(path, "/hci")) == NULL || path[4] == '\0')
		return -1;
	return atoi(&path[4]);
}

/**
 * Extract BT address from the BlueZ D-Bus object path.
 *
 * @param path BlueZ D-Bus object path.
 * @param addr Address where the parsed BT address will be stored.
 * @return On success this function returns pointer to the BT address. On
 *   error, NULL is returned. */
bdaddr_t *g_dbus_bluez_object_path_to_bdaddr(const char *path, bdaddr_t *addr) {

	char tmp[sizeof("00:00:00:00:00:00")] = { 0 };

	if ((path = strstr(path, "/dev_")) != NULL)
		strncpy(tmp, path + 5, sizeof(tmp) - 1);

	for (size_t i = 0; i < sizeof(tmp); i++)
		if (tmp[i] == '_')
			tmp[i] = ':';

	if (str2ba(tmp, addr) == -1)
		return NULL;

	return addr;
}

/**
 * Sanitize D-Bus object path.
 *
 * @param path D-Bus object path.
 * @return Pointer to the object path string. */
char * g_variant_sanitize_object_path(char * path) {

	char *tmp = path - 1;

	while (*(++tmp) != '\0')
		if (!(*tmp == '/' || isalnum(*tmp)))
			*tmp = '_';

	return path;
}

/**
 * Create a new byte array GVariant from raw data.
 *
 * @param data Pointer to the raw data.
 * @param size Size of the data in bytes.
 * @return New GVariant byte array. */
GVariant * g_variant_new_fixed_byte_array(const void * data, size_t len) {
	return g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, data, len, sizeof(uint8_t));
}

/**
 * Convenience wrapper around g_variant_is_of_type().
 *
 * @param value Variant for validation.
 * @param type Expected variant type.
 * @param name Variant name for logging.
 * @return If variant matches type, this function returns true. */
bool g_variant_validate_value(GVariant * value, const GVariantType * type,
		const char * name) {
	if (g_variant_is_of_type(value, type))
		return true;
	warn("Invalid variant type: %s: %s != %s", name,
			g_variant_get_type_string(value), (const char *)type);
	return false;
}

/**
 * Create a new IO channel for raw (unbuffered, no encoding) access.
 *
 * The returned IO channel takes ownership of the given file descriptor - the
 * file descriptor will be closed when the channel is freed. Users can disable
 * this by calling g_io_channel_set_close_on_unref() on the returned channel.
 *
 * @param fd File descriptor.
 * @return New raw IO channel. */
GIOChannel * g_io_channel_unix_raw_new(int fd) {
	GIOChannel * ch = g_io_channel_unix_new(fd);
	g_io_channel_set_close_on_unref(ch, TRUE);
	g_io_channel_set_encoding(ch, NULL, NULL);
	g_io_channel_set_buffered(ch, FALSE);
	return ch;
}

/**
 * Create a new watch source for the given I/O channel.
 *
 * @param channel A pointer to the GIOChannel.
 * @param priority The priority of the source.
 * @param cond The condition to watch for.
 * @param func The function to call when the condition is satisfied.
 * @param userdata Data to pass to the function.
 * @param notify Function to call when the source is destroyed.
 * @return New watch source. */
GSource * g_io_create_watch_full(GIOChannel * channel, int priority,
		GIOCondition cond, GIOFunc func, void * userdata, GDestroyNotify notify) {
	GSource * watch = g_io_create_watch(channel, cond);
	g_source_set_callback(watch, G_SOURCE_FUNC(func), userdata, notify);
	g_source_set_priority(watch, priority);
	g_source_attach(watch, NULL);
	return watch;
}

/**
 * Convert a pointer to BT address to a hash value.
 *
 * @param v A pointer to bdaddr_t structure.
 * @return Hash value compatible with GHashTable. */
unsigned int g_bdaddr_hash(const void *v) {
	const uint16_t *b = (uint16_t *)((const bdaddr_t *)v)->b;
	return (b[0] | ((uint32_t)b[1]) << 16) * b[2];
}

/**
 * Compare two BT addresses.
 *
 * @param v1 A pointer to first bdaddr_t structure.
 * @param v2 A pointer to second bdaddr_t structure.
 * @return Comparision value compatible with GHashTable. */
gboolean g_bdaddr_equal(const void *v1, const void *v2) {
	return bacmp(v1, v2) == 0;
}

#if ENABLE_MP3LAME
/**
 * Get maximum possible bitrate for the given bitrate mask.
 *
 * @param mask MPEG-1 layer III bitrate mask.
 * @return Bitrate in kilobits per second. */
int a2dp_mpeg1_mp3_get_max_bitrate(uint16_t mask) {

	static int bitrates[] = { 320, 256, 224, 192, 160, 128, 112, 96, 80, 64, 56, 48, 40, 32 };
	size_t i = 0;

	while (i < ARRAYSIZE(bitrates)) {
		if (mask & (1 << (14 - i)))
			return bitrates[i];
		i++;
	}

	return -1;
}
#endif
