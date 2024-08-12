/*
 * mock.h
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <gio/gio.h>
#include <glib.h>

#define MOCK_ADAPTER_ID 11
#define MOCK_ADAPTER_ADDRESS "00:00:11:11:22:22"
#define MOCK_DEVICE_1 "12:34:56:78:9A:BC"
#define MOCK_DEVICE_2 "23:45:67:89:AB:CD"

#define MOCK_BLUEZ_ADAPTER_PATH "/org/bluez/hci11"
#define MOCK_BLUEZ_DEVICE_1_PATH MOCK_BLUEZ_ADAPTER_PATH "/dev_12_34_56_78_9A_BC"
#define MOCK_BLUEZ_DEVICE_1_SEP_PATH MOCK_BLUEZ_DEVICE_1_PATH "/sep"
#define MOCK_BLUEZ_DEVICE_2_PATH MOCK_BLUEZ_ADAPTER_PATH "/dev_23_45_67_89_AB_CD"
#define MOCK_BLUEZ_DEVICE_2_SEP_PATH MOCK_BLUEZ_DEVICE_2_PATH "/sep"
#define MOCK_BLUEZ_MIDI_PATH MOCK_BLUEZ_ADAPTER_PATH "/MIDI"

struct MockService {

	const char *name;
	void (*name_acquired_cb)(GDBusConnection *conn, const char *name, void *userdata);
	void (*name_lost_cb)(GDBusConnection *conn, const char *name, void *userdata);

	GThread *thread;
	GAsyncQueue *ready;
	GMainLoop *loop;
	unsigned int id;

};

extern GAsyncQueue *mock_sem_ready;
extern GAsyncQueue *mock_sem_timeout;
extern char mock_ba_service_name[32];
extern bool mock_dump_output;
extern int mock_fuzzing_ms;

void mock_bluealsa_run(void);
void mock_bluealsa_service_start(void);
void mock_bluealsa_service_stop(void);

int mock_bluez_device_name_mapping_add(const char *mapping);
int mock_bluez_device_media_endpoint_add(const char *endpoint_path,
		const char *device_path, const char *uuid, uint32_t codec_id,
		const void *capabilities, size_t capabilities_size);
int mock_bluez_device_profile_new_connection(const char *device_path,
		const char *uuid, GAsyncQueue *sem_ready);
int mock_bluez_device_media_set_configuration(const char *device_path,
		const char *transport_path, const char *uuid, uint32_t codec_id,
		const void *configuration, size_t configuration_size,
		GAsyncQueue *sem_ready);
void mock_bluez_service_start(void);
void mock_bluez_service_stop(void);

void mock_ofono_service_start(void);
void mock_ofono_service_stop(void);

int mock_upower_display_device_set_is_present(bool present);
int mock_upower_display_device_set_percentage(double percentage);
void mock_upower_service_start(void);
void mock_upower_service_stop(void);

void mock_sem_signal(GAsyncQueue *sem);
void mock_sem_wait(GAsyncQueue *sem);

void mock_service_start(struct MockService *service);
void mock_service_stop(struct MockService *service);

GThread *mock_bt_dump_thread_new(int fd);
