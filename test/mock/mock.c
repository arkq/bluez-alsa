/*
 * mock.c
 * SPDX-FileCopyrightText: 2016-2025 BlueALSA developers
 * SPDX-License-Identifier: MIT
 *
 * This program might be used to debug or check the functionality of ALSA
 * plug-ins. It should work exactly the same as the BlueALSA server.
 *
 */

#include "mock.h"
#include "service.h"

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <libgen.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gio/gio.h>
#include <glib-unix.h>
#include <glib.h>

#include "a2dp.h"
#include "ba-config.h"
#include "bluealsa-iface.h"
#include "dbus.h"
#include "ofono.h"
#include "storage.h"
#include "shared/a2dp-codecs.h"
#include "shared/defs.h"
#include "shared/log.h"

/* Keep persistent storage in the current directory. */
#define TEST_BLUEALSA_STORAGE_DIR "storage-mock"

static void * main_loop_run(void * userdata) {
	g_main_loop_run(userdata);
	return NULL;
}

static int queue_push_callback(void * userdata) {
	g_async_queue_push(userdata, GINT_TO_POINTER(1));
	return G_SOURCE_REMOVE;
}

int main(int argc, char *argv[]) {

	int opt;
	const char * opts = "hB:p:c:t:";
	struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "dbus", required_argument, NULL, 'B' },
		{ "profile", required_argument, NULL, 'p' },
		{ "codec", required_argument, NULL, 'c' },
		{ "timeout", required_argument, NULL, 't' },
		{ "device-name", required_argument, NULL, 2 },
		{ "fuzzing", required_argument, NULL, 7 },
		{ 0, 0, 0, 0 },
	};

	char ba_service_name[32] = BLUEALSA_SERVICE;
	int timeout_ms = 5000;
	int fuzzing_ms = 0;

	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1)
		switch (opt) {
		case 'h':
			printf("Usage:\n"
					"  %s [OPTION]...\n"
					"\nOptions:\n"
					"  -h, --help\t\t\tprint this help and exit\n"
					"  -B, --dbus=NAME\t\tBlueALSA service name suffix\n"
					"  -p, --profile=NAME\t\tset enabled BT profiles\n"
					"  -c, --codec=NAME\t\tset enabled BT audio codecs\n"
					"  -t, --timeout=MSEC\t\tmock server exit timeout\n"
					"  --device-name=MAC:NAME\tmock BT device name\n"
					"  --fuzzing=MSEC\t\tmock human actions with timings\n",
					argv[0]);
			return EXIT_SUCCESS;
		case 'B' /* --dbus=NAME */ :
			snprintf(ba_service_name, sizeof(ba_service_name),
					BLUEALSA_SERVICE ".%s", optarg);
			break;
		case 'p' /* --profile=NAME */ : {

			static const struct {
				const char * name;
				bool * ptr;
			} map[] = {
				{ "a2dp-source", &config.profile.a2dp_source },
				{ "a2dp-sink", &config.profile.a2dp_sink },
#if ENABLE_ASHA
				{ "asha-source", &config.profile.asha_source },
				{ "asha-sink", &config.profile.asha_sink },
#endif
#if ENABLE_OFONO
				{ "hfp-ofono", &config.profile.hfp_ofono },
#endif
				{ "hfp-ag", &config.profile.hfp_ag },
				{ "hfp-hf", &config.profile.hfp_hf },
				{ "hsp-ag", &config.profile.hsp_ag },
				{ "hsp-hs", &config.profile.hsp_hs },
#if ENABLE_MIDI
				{ "midi", &config.profile.midi },
#endif
			};

			bool matched = false;
			for (size_t i = 0; i < ARRAYSIZE(map); i++)
				if (strcasecmp(optarg, map[i].name) == 0) {
					*map[i].ptr = true;
					matched = true;
					break;
				}

			if (!matched) {
				error("Invalid BT profile name: %s", optarg);
				return EXIT_FAILURE;
			}

			break;
		}
		case 'c' /* --codec=NAME */ : {

			uint32_t codec_id = a2dp_codecs_codec_id_from_string(optarg);
			bool matched = false;

			struct a2dp_sep * const * seps = a2dp_seps;
			for (struct a2dp_sep *sep = *seps; sep != NULL; sep = *++seps)
				if (sep->config.codec_id == codec_id) {
					sep->enabled = true;
					matched = true;
				}

			if (!matched) {
				error("Invalid BT codec name: %s", optarg);
				return EXIT_FAILURE;
			}

			break;
		}
		case 't' /* --timeout=MSEC */ :
			timeout_ms = atoi(optarg);
			break;
		case 2 /* --device-name=MAC:NAME */ :
			mock_bluez_service_add_device_name_mapping(optarg);
			break;
		case 7 /* --fuzzing=MSEC */ :
			fuzzing_ms = atoi(optarg);
			break;
		default:
			fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
			return EXIT_FAILURE;
		}

	log_open(basename(argv[0]), false);
	assert(ba_config_init() == 0);

	/* Add BT address to the HCI filter to test filtering logic. */
	const char *filter = MOCK_ADAPTER_ADDRESS;
	g_array_append_val(config.hci_filter, filter);

	assert(mkdir(TEST_BLUEALSA_STORAGE_DIR, 0755) == 0 || errno == EEXIST);
	assert(storage_init(TEST_BLUEALSA_STORAGE_DIR) == 0);
	atexit(storage_destroy);

	g_autoptr(GTestDBus) dbus = g_test_dbus_new(G_TEST_DBUS_NONE);
	g_test_dbus_up(dbus);

	const char * dbus_address = g_test_dbus_get_bus_address(dbus);
	fprintf(stderr, "DBUS_SYSTEM_BUS_ADDRESS=%s\n", dbus_address);

	/* receive EPIPE error code */
	struct sigaction sigact = { .sa_handler = SIG_IGN };
	sigaction(SIGPIPE, &sigact, NULL);

	/* Timeout queue. */
	g_autoptr(GAsyncQueue) queue = g_async_queue_new();

	/* Set up main loop with graceful termination handlers. */
	g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
	GThread * loop_th = g_thread_new(NULL, main_loop_run, loop);
	g_unix_signal_add(SIGINT, queue_push_callback, queue);
	g_unix_signal_add(SIGTERM, queue_push_callback, queue);

	g_autoptr(BlueZMockService) bluez = mock_bluez_service_new();
	bluez->media_transport_update_ms = fuzzing_ms;
	g_autoptr(GDBusConnection) conn1 = g_dbus_connection_new_for_address_simple_sync(
			dbus_address, NULL);
	mock_service_start(bluez, conn1);

	g_autoptr(OFonoMockService) ofono = mock_ofono_service_new();
	g_autoptr(GDBusConnection) conn2 = g_dbus_connection_new_for_address_simple_sync(
			dbus_address, NULL);
	mock_service_start(ofono, conn2);

	g_autoptr(UPowerMockService) upower = mock_upower_service_new();
	g_autoptr(GDBusConnection) conn3 = g_dbus_connection_new_for_address_simple_sync(
			dbus_address, NULL);
	mock_service_start(upower, conn3);

	/* Start BlueALSA as the last service. */
	g_autoptr(BlueALSAMockService) ba = mock_bluealsa_service_new(
			ba_service_name, bluez, ofono, upower);
	ba->fuzzing_ms = fuzzing_ms;
	g_autoptr(GDBusConnection) conn4 = g_dbus_connection_new_for_address_simple_sync(
			dbus_address, NULL);
	mock_service_start(ba, conn4);

#if ENABLE_OFONO
	assert(ofono_detect_service() == true);
#endif

	/* Start the termination timer after all services are up and running. */
	g_timeout_add(timeout_ms, queue_push_callback, queue);
	/* Run mock until timeout or SIGINT/SIGTERM signal. */
	mock_bluealsa_service_run(ba, queue);

	mock_service_stop(ofono);
	mock_service_stop(upower);
	/* Simulate BlueZ termination while BlueALSA is still running. */
	mock_service_stop(bluez);
	mock_service_stop(ba);

	g_main_loop_quit(loop);
	g_thread_join(loop_th);

	return EXIT_SUCCESS;
}
