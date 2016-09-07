/*
 * utils.inc
 * vim: ft=c
 *
 * Copyright (c) 2016 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/**
 * Helper function for loading file content into the buffer. */
static int load_file(const char *path, char **buffer, size_t *size) {

	FILE *f;

	if ((f = fopen(path, "rb")) == NULL)
		return -errno;

	*size = lseek(fileno(f), 0, SEEK_END);
	*buffer = malloc(*size);

	rewind(f);
	*size = fread(*buffer, 1, *size, f);

	return fclose(f);
}
