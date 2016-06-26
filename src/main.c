/*
 * bluealsa - main.c
 * Copyright (c) 2016 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>

#include "services.h"


int main(int argc, char **argv) {

	int opt;
	const char *opts = "h";
	struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ 0, 0, 0, 0 },
	};

	/* parse options */
	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1)
		switch (opt) {
		case 'h':
			printf("usage: %s [ -h ]\n"
					"\n"
					"  -h, --help\t\tprint this help and exit\n",
					argv[0]);
			return EXIT_SUCCESS;

		default:
			fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
			return EXIT_FAILURE;
		}

	return EXIT_SUCCESS;
}
