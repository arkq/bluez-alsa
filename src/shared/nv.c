/*
 * BlueALSA - nv.h
 * SPDX-FileCopyrightText: 2022-2025 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#include "nv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/**
 * Get the first entry matching the given name.
 *
 * @param entries NULL-terminated array of entries.
 * @param name The entry name to search for.
 * @return On success this function returns the address of the entry from
 *   the given entry list. If the name was not found, NULL is returned. */
nv_entry_t * nv_lookup_entry(const nv_entry_t * entries, const char * name) {
	for (const nv_entry_t * e = entries; e->name != NULL; e++)
		if (strcasecmp(name, e->name) == 0)
			return (nv_entry_t *)e;
	return NULL;
}

/**
 * Get the name of the first entry matching the given integer value.
 *
 * @param entries NULL-terminated array of entries.
 * @param value The integer value to search for.
 * @return On success this function returns the name associated with
 *   the given value. If the value was not found, NULL is returned. */
const char * nv_name_from_int(const nv_entry_t * entries, int value) {
	for (const nv_entry_t * e = entries; e->name != NULL; e++)
		if (e->v.i == value)
			return e->name;
	return NULL;
}

/**
 * Get the name of the first entry matching the given unsigned integer value.
 *
 * @param entries NULL-terminated array of entries.
 * @param value The unsigned integer value to search for.
 * @return On success this function returns the name associated with
 *   the given value. If the value was not found, NULL is returned. */
const char * nv_name_from_uint(const nv_entry_t * entries, unsigned int value) {
	for (const nv_entry_t * e = entries; e->name != NULL; e++)
		if (e->v.u == value)
			return e->name;
	return NULL;
}

/**
 * Get entry names joined with a comma.
 *
 * @param entries NULL-terminated array of entries.
 * @return On success this function returns a string with all entry names
 *   joined with a comma. The returned string shall be freed with free().
 *   If an error has occurred, NULL is returned and errno is set to
 *   indicate the error. */
char * nv_join_names(const nv_entry_t * entries) {

	size_t size = 0;
	char * buffer;

	/* calculate required buffer size */
	for (const nv_entry_t * e = entries; e->name != NULL; e++)
		size += strlen(e->name) + 2;

	if ((buffer = malloc(size + 1)) == NULL)
		return NULL;

	char * tmp = buffer;
	for (const nv_entry_t * e = entries; e->name != NULL; e++)
		tmp += sprintf(tmp, "%s, ", e->name);

	if (size != 0)
		tmp -= 2;

	/* add terminating null byte */
	* tmp = '\0';

	return buffer;
}
