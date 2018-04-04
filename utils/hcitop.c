/*
 * BlueALSA - hcitop.c
 * Copyright (c) 2016-2018 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#if HAVE_CONFIG_H
# include "config.h"
#endif

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <ncurses.h>
#include <bsd/stdlib.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

static const struct {
	unsigned int bit;
	char flag;
} hci_flags_map[] = {
	{ HCI_UP, 'U' },
	{ HCI_INIT, 'I' },
	{ HCI_RUNNING, 'R' },
	{ HCI_PSCAN, 'P' },
	{ HCI_ISCAN, 'I' },
	{ HCI_AUTH, 'A' },
	{ HCI_ENCRYPT, 'E' },
	{ HCI_INQUIRY, 'Q' },
	{ HCI_RAW, 'X' },
};

static int get_devinfo(struct hci_dev_info di[HCI_MAX_DEV]) {

	int i, num;

	for (i = num = 0; i < HCI_MAX_DEV; i++)
		if (hci_devinfo(i, &di[num]) == 0)
			num++;

	return num;
}

static unsigned int get_average_rate(unsigned int *array, size_t size) {

	/* at least two points are required */
	if (size < 2)
		return 0;

	unsigned int x = 0;
	unsigned int y = 0;
	size_t i;

	size--;
	i = size;

	while (i--) {
		int b = (array[i] - array[i + 1]) % size;
		x += (array[i] - array[i + 1]) / size;
		if (y >= size - b) {
			y -= size - b;
			x++;
		}
		else {
			y += b;
		}
	}

	return x + y / size;
}

static void sprint_hci_flags(char *str, unsigned int flags) {

	size_t i;

	for (i = 0; i < sizeof(hci_flags_map) / sizeof(*hci_flags_map); i++)
		str[i] = hci_test_bit(hci_flags_map[i].bit, &flags) ? hci_flags_map[i].flag : ' ';

	str[i] = '\0';
}

int main(int argc, char *argv[]) {

	int opt;
	const char *opts = "hVd:";
	const struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "version", no_argument, NULL, 'V' },
		{ "delay", required_argument, NULL, 'd' },
		{ 0, 0, 0, 0 },
	};

	int delay_sec = 1;
	int delay_msec = 0;

	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1)
		switch (opt) {
		case 'h' /* --help */ :
			printf("usage: %s [ -d sec ]\n"
					"  -h, --help\t\tprint this help and exit\n"
					"  -V, --version\t\tprint version and exit\n"
					"  -d, --delay=SEC\tdelay time interval\n",
					argv[0]);
			return EXIT_SUCCESS;

		case 'V' /* --version */ :
			printf("%s\n", PACKAGE_VERSION);
			return EXIT_SUCCESS;

		case 'd' /* --delay=SEC */ :
			delay_sec = atoi(optarg);
			delay_msec = (int)((atof(optarg) - delay_sec) * 10) * 100;
			if (delay_sec < 0 || delay_msec < 0 || (delay_sec == 0 && delay_msec == 0)) {
				fprintf(stderr, "%s: -d requires positive argument (max precision: 0.1)\n", argv[0]);
				return EXIT_FAILURE;
			}
			break;

		default:
			fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
			return EXIT_FAILURE;
		}

	struct hci_dev_info devices[HCI_MAX_DEV];
	unsigned int byte_rx[HCI_MAX_DEV][3];
	unsigned int byte_tx[HCI_MAX_DEV][3];
	size_t ii;

	memset(byte_rx, 0, sizeof(byte_rx));
	memset(byte_tx, 0, sizeof(byte_tx));

	initscr();
	cbreak();
	noecho();
	curs_set(0);

	for (ii = 1;; ii++) {

		const char *template_top = "%5s %9s %8s %8s %8s %8s";
		const char *template_row = "%5s %9s %8s %8s %8s %8s";
		int i, count;

		attron(A_REVERSE);
		mvprintw(0, 0, template_top, "HCI", "FLAGS", "RX", "TX", "RX/s", "TX/s");
		attroff(A_REVERSE);

		count = get_devinfo(devices);
		for (i = 0; i < HCI_MAX_DEV; i++) {

			/* shift historic data to the right by one sample */
			memmove(&byte_rx[i][1], &byte_rx[i][0], sizeof(*byte_rx) - sizeof(**byte_rx));
			memmove(&byte_tx[i][1], &byte_tx[i][0], sizeof(*byte_tx) - sizeof(**byte_tx));

			if (i >= count)
				continue;

			char flags[sizeof(hci_flags_map) / sizeof(*hci_flags_map) + 1];

			sprint_hci_flags(flags, devices[i].flags);

			byte_rx[i][0] = devices[i].stat.byte_rx;
			byte_tx[i][0] = devices[i].stat.byte_tx;

			const size_t ii_max = sizeof(*byte_rx) / sizeof(**byte_rx);
			const size_t samples = ii < ii_max ? ii : ii_max;
			unsigned int rate_rx = get_average_rate(byte_rx[i], samples);
			unsigned int rate_tx = get_average_rate(byte_tx[i], samples);

			rate_rx = rate_rx * 10 / (delay_sec * 10 + delay_msec / 100);
			rate_tx = rate_tx * 10 / (delay_sec * 10 + delay_msec / 100);

			char rx[7], rx_rate[9];
			char tx[7], tx_rate[9];

			humanize_number(rx, sizeof(rx), byte_rx[i][0], "B", HN_AUTOSCALE, 0);
			humanize_number(tx, sizeof(tx), byte_tx[i][0], "B", HN_AUTOSCALE, 0);
			humanize_number(rx_rate, sizeof(rx_rate), rate_rx, "B", HN_AUTOSCALE, 0);
			humanize_number(tx_rate, sizeof(tx_rate), rate_tx, "B", HN_AUTOSCALE, 0);

			mvprintw(i + 1, 0, template_row, devices[i].name, flags, rx, tx, rx_rate, tx_rate);
		}

		timeout(delay_sec * 1000 + delay_msec);
		if (getch() == 'q')
			break;

	}

	endwin();
	return EXIT_SUCCESS;
}
