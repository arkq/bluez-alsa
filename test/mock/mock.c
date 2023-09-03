/*
 * mock.c
 * Copyright (c) 2016-2023 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 * This program might be used to debug or check the functionality of ALSA
 * plug-ins. It should work exactly the same as the BlueALSA server.
 *
 */

#include "mock.h"

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <gio/gio.h>
#include <glib-unix.h>
#include <glib.h>

#include "a2dp.h"
#include "ba-adapter.h"
#include "bluealsa-config.h"
#include "bluealsa-dbus.h"
#include "bluealsa-iface.h"
#include "bluez-iface.h"
#include "storage.h"
#include "shared/a2dp-codecs.h"
#include "shared/defs.h"
#include "shared/log.h"

#define TEST_BLUEALSA_STORAGE_DIR "/tmp/bluealsa-mock-storage"

struct ba_adapter *mock_adapter = NULL;
GAsyncQueue *mock_sem_timeout = NULL;
GAsyncQueue *mock_sem_quit = NULL;
bool mock_dump_output = false;
int mock_fuzzing_ms = 0;

void mock_sem_signal(GAsyncQueue *sem) {
	g_async_queue_push(sem, GINT_TO_POINTER(1));
}

void mock_sem_wait(GAsyncQueue *sem) {
	g_async_queue_pop(sem);
}

static void *mock_main_loop_run(void *userdata) {
	g_main_loop_run((GMainLoop *)userdata);
	return NULL;
}

static int mock_sem_signal_handler(void *userdata) {
	mock_sem_signal((GAsyncQueue *)userdata);
	return G_SOURCE_REMOVE;
}

int main(int argc, char *argv[]) {

	int opt;
	const char *opts = "hB:p:c:t:";
	struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "dbus", required_argument, NULL, 'B' },
		{ "profile", required_argument, NULL, 'p' },
		{ "codec", required_argument, NULL, 'c' },
		{ "timeout", required_argument, NULL, 't' },
		{ "device-name", required_argument, NULL, 2 },
		{ "dump-output", no_argument, NULL, 6 },
		{ "fuzzing", required_argument, NULL, 7 },
		{ 0, 0, 0, 0 },
	};

	char ba_service[32] = BLUEALSA_SERVICE;
	int timeout_ms = 5000;

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
					"  --dump-output\t\t\tdump Bluetooth transport data\n"
					"  --fuzzing=MSEC\t\tmock human actions with timings\n",
					argv[0]);
			return EXIT_SUCCESS;
		case 'B' /* --dbus=NAME */ :
			snprintf(ba_service, sizeof(ba_service), BLUEALSA_SERVICE ".%s", optarg);
			break;
		case 'p' /* --profile=NAME */ : {

			static const struct {
				const char *name;
				bool *ptr;
			} map[] = {
				{ "a2dp-source", &config.profile.a2dp_source },
				{ "a2dp-sink", &config.profile.a2dp_sink },
				{ "hfp-ag", &config.profile.hfp_ag },
				{ "hsp-ag", &config.profile.hsp_ag },
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

			uint16_t codec_id = a2dp_codecs_codec_id_from_string(optarg);
			bool matched = false;

			struct a2dp_codec * const * cc = a2dp_codecs;
			for (struct a2dp_codec *c = *cc; c != NULL; c = *++cc)
				if (c->codec_id == codec_id) {
					c->enabled = true;
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
			mock_bluez_device_name_mapping_add(optarg);
			break;
		case 6 /* --dump-output */ :
			mock_dump_output = true;
			break;
		case 7 /* --fuzzing=MSEC */ :
			mock_fuzzing_ms = atoi(optarg);
			break;
		default:
			fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
			return EXIT_FAILURE;
		}

	log_open(basename(argv[0]), false);
	assert(bluealsa_config_init() == 0);

	assert(mkdir(TEST_BLUEALSA_STORAGE_DIR, 0755) == 0 || errno == EEXIST);
	assert(storage_init(TEST_BLUEALSA_STORAGE_DIR) == 0);

	GTestDBus *dbus = g_test_dbus_new(G_TEST_DBUS_NONE);
	g_test_dbus_up(dbus);

	fprintf(stderr, "DBUS_SYSTEM_BUS_ADDRESS=%s\n", g_test_dbus_get_bus_address(dbus));
	assert((config.dbus = g_dbus_connection_new_for_address_sync(
					g_test_dbus_get_bus_address(dbus),
					G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
					G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
					NULL, NULL, NULL)) != NULL);

	/* receive EPIPE error code */
	struct sigaction sigact = { .sa_handler = SIG_IGN };
	sigaction(SIGPIPE, &sigact, NULL);

	/* thread synchronization queues (semaphores) */
	mock_sem_timeout = g_async_queue_new();
	mock_sem_quit = g_async_queue_new();

	/* main loop with graceful termination handlers */
	GMainLoop *loop = g_main_loop_new(NULL, FALSE);
	GThread *loop_th = g_thread_new(NULL, mock_main_loop_run, loop);
	g_timeout_add(timeout_ms, mock_sem_signal_handler, mock_sem_timeout);
	g_unix_signal_add(SIGINT, mock_sem_signal_handler, mock_sem_quit);
	g_unix_signal_add(SIGTERM, mock_sem_signal_handler, mock_sem_quit);

	bluealsa_dbus_register();

	assert(g_bus_own_name_on_connection(config.dbus, ba_service,
				G_BUS_NAME_OWNER_FLAGS_NONE, mock_bluealsa_dbus_name_acquired, NULL,
				NULL, NULL) != 0);
	assert(g_bus_own_name_on_connection(config.dbus, BLUEZ_SERVICE,
				G_BUS_NAME_OWNER_FLAGS_NONE, mock_bluez_dbus_name_acquired, NULL,
				NULL, NULL) != 0);

	/* run mock until timeout or SIGINT/SIGTERM */
	mock_sem_wait(mock_sem_quit);

	ba_adapter_destroy(mock_adapter);

	g_main_loop_quit(loop);
	g_main_loop_unref(loop);
	g_thread_join(loop_th);

	return EXIT_SUCCESS;
}
