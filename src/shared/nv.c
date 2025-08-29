/*
 * BlueALSA - nv.h
 * Copyright (c) 2016-2022 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "nv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/**
 * Find entry in the NULL-terminated entry list.
 *
 * @param entries NULL-terminated array of entries.
 * @param name The name of searched entry.
 * @return On success this function returns the address of the entry from
 *   the given entry list. If the name was not found, it returns NULL. */
nv_entry_t *nv_find(const nv_entry_t *entries, const char *name) {
	for (const nv_entry_t *e = entries; e->name != NULL; e++)
		if (strcasecmp(name, e->name) == 0)
			return (nv_entry_t *)e;
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
char *nv_join_names(const nv_entry_t *entries) {

	size_t size = 0;
	char *buffer;

	/* calculate required buffer size */
	for (const nv_entry_t *e = entries; e->name != NULL; e++)
		size += strlen(e->name) + 2;

	if ((buffer = malloc(size + 1)) == NULL)
		return NULL;

	char *tmp = buffer;
	for (const nv_entry_t *e = entries; e->name != NULL; e++)
		tmp += sprintf(tmp, "%s, ", e->name);

	if (size != 0)
		tmp -= 2;

	/* add terminating null byte */
	*tmp = '\0';

	return buffer;
}
