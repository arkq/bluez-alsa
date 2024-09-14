/*
 * mock.c
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
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
#include "ofono.h"
#include "storage.h"
#include "shared/a2dp-codecs.h"
#include "shared/defs.h"
#include "shared/log.h"

/* Keep persistent storage in the current directory. */
#define TEST_BLUEALSA_STORAGE_DIR "storage-mock"

GAsyncQueue *mock_sem_ready = NULL;
GAsyncQueue *mock_sem_timeout = NULL;
char mock_ba_service_name[32] = BLUEALSA_SERVICE;
bool mock_dump_output = false;
int mock_fuzzing_ms = 0;

static const char *dbus_address = NULL;
static GDBusConnection *mock_dbus_connection_new_sync(GError **error) {
	return g_dbus_connection_new_for_address_sync(dbus_address,
			G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
			G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
			NULL, NULL, error);
}

static void *mock_loop_run(void *userdata) {
	struct MockService *service = userdata;

	g_autoptr(GMainContext) context = g_main_context_new();
	service->loop = g_main_loop_new(context, FALSE);
	g_main_context_push_thread_default(context);

	g_autoptr(GDBusConnection) conn = mock_dbus_connection_new_sync(NULL);
	g_assert((service->id = g_bus_own_name_on_connection(conn, service->name,
					G_BUS_NAME_OWNER_FLAGS_NONE, service->name_acquired_cb, service->name_lost_cb,
					userdata, NULL)) != 0);

	g_main_loop_run(service->loop);

	g_main_context_pop_thread_default(context);
	return NULL;
}

void mock_sem_signal(GAsyncQueue *sem) {
	g_async_queue_push(sem, GINT_TO_POINTER(1));
}

void mock_sem_wait(GAsyncQueue *sem) {
	g_async_queue_pop(sem);
}

void mock_service_start(struct MockService *service) {
	service->ready = g_async_queue_new();
	service->thread = g_thread_new(service->name, mock_loop_run, service);
	mock_sem_wait(service->ready);
	g_async_queue_unref(service->ready);
}

void mock_service_stop(struct MockService *service) {

	g_bus_unown_name(service->id);

	g_main_loop_quit(service->loop);
	g_main_loop_unref(service->loop);
	g_thread_join(service->thread);

}

static void *mock_bt_dump_thread(void *userdata) {

	int bt_fd = GPOINTER_TO_INT(userdata);
	FILE *f_output = NULL;
	uint8_t buffer[1024];
	ssize_t len;

	if (mock_dump_output)
		f_output = fopen("bluealsad-mock.dump", "w");

	debug("IO loop: START: %s", __func__);
	while ((len = read(bt_fd, buffer, sizeof(buffer))) > 0) {
		fprintf(stderr, "#");

		if (!mock_dump_output)
			continue;

		for (ssize_t i = 0; i < len; i++)
			fprintf(f_output, "%02x", buffer[i]);
		fprintf(f_output, "\n");

	}

	debug("IO loop: EXIT: %s", __func__);
	if (f_output != NULL)
		fclose(f_output);
	close(bt_fd);
	return NULL;
}

GThread *mock_bt_dump_thread_new(int fd) {
	return g_thread_new(NULL, mock_bt_dump_thread, GINT_TO_POINTER(fd));
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
			snprintf(mock_ba_service_name, sizeof(mock_ba_service_name),
					BLUEALSA_SERVICE ".%s", optarg);
			break;
		case 'p' /* --profile=NAME */ : {

			static const struct {
				const char *name;
				bool *ptr;
			} map[] = {
				{ "a2dp-source", &config.profile.a2dp_source },
				{ "a2dp-sink", &config.profile.a2dp_sink },
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
	assert(ba_config_init() == 0);

	/* Add BT address to the HCI filter to test filtering logic. */
	const char *filter = MOCK_ADAPTER_ADDRESS;
	g_array_append_val(config.hci_filter, filter);

	assert(mkdir(TEST_BLUEALSA_STORAGE_DIR, 0755) == 0 || errno == EEXIST);
	assert(storage_init(TEST_BLUEALSA_STORAGE_DIR) == 0);
	atexit(storage_destroy);

	g_autoptr(GTestDBus) dbus = g_test_dbus_new(G_TEST_DBUS_NONE);
	g_test_dbus_up(dbus);

	dbus_address = g_test_dbus_get_bus_address(dbus);
	fprintf(stderr, "DBUS_SYSTEM_BUS_ADDRESS=%s\n", dbus_address);

	/* receive EPIPE error code */
	struct sigaction sigact = { .sa_handler = SIG_IGN };
	sigaction(SIGPIPE, &sigact, NULL);

	/* thread synchronization queues (semaphores) */
	mock_sem_ready = g_async_queue_new();
	mock_sem_timeout = g_async_queue_new();

	/* Set up main loop with graceful termination handlers. */
	g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
	GThread *loop_th = g_thread_new(NULL, mock_main_loop_run, loop);
	g_unix_signal_add(SIGINT, mock_sem_signal_handler, mock_sem_timeout);
	g_unix_signal_add(SIGTERM, mock_sem_signal_handler, mock_sem_timeout);

	mock_bluez_service_start();
	mock_ofono_service_start();
	mock_upower_service_start();
	/* Start BlueALSA as the last service. */
	mock_bluealsa_service_start();

#if ENABLE_OFONO
	assert(ofono_detect_service() == true);
#endif

	/* Start the termination timer after all services are up and running. */
	g_timeout_add(timeout_ms, mock_sem_signal_handler, mock_sem_timeout);

	/* Run mock until timeout or SIGINT/SIGTERM signal. */
	mock_bluealsa_run();

	mock_ofono_service_stop();
	mock_upower_service_stop();
	/* Simulate BlueZ termination while BlueALSA is still running. */
	mock_bluez_service_stop();
	mock_bluealsa_service_stop();

	g_main_loop_quit(loop);
	g_thread_join(loop_th);

	return EXIT_SUCCESS;
}
