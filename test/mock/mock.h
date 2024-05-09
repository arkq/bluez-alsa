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

#define MOCK_ADAPTER_ID 0
#define MOCK_ADAPTER_ADDRESS "00:00:11:11:22:22"
#define MOCK_DEVICE_1 "12:34:56:78:9A:BC"
#define MOCK_DEVICE_2 "23:45:67:89:AB:CD"

#define MOCK_BLUEZ_ADAPTER_PATH "/org/bluez/hci0"
#define MOCK_BLUEZ_DEVICE_PATH_1 MOCK_BLUEZ_ADAPTER_PATH "/dev_12_34_56_78_9A_BC"
#define MOCK_BLUEZ_DEVICE_PATH_2 MOCK_BLUEZ_ADAPTER_PATH "/dev_23_45_67_89_AB_CD"
#define MOCK_BLUEZ_MIDI_PATH_1 MOCK_BLUEZ_ADAPTER_PATH "/MIDI"
#define MOCK_BLUEZ_SCO_PATH_1 MOCK_BLUEZ_DEVICE_PATH_1 "/sco"
#define MOCK_BLUEZ_SCO_PATH_2 MOCK_BLUEZ_DEVICE_PATH_2 "/sco"

extern GAsyncQueue *mock_sem_ready;
extern GAsyncQueue *mock_sem_timeout;
extern char mock_ba_service_name[32];
extern bool mock_dump_output;
extern int mock_fuzzing_ms;

GDBusConnection *mock_dbus_connection_new_sync(GError **error);

void mock_bluealsa_run(void);
void mock_bluealsa_service_start(void);
void mock_bluealsa_service_stop(void);

int mock_bluez_device_name_mapping_add(const char *mapping);
int mock_bluez_device_profile_new_connection(const char *device_path,
		const char *uuid, GAsyncQueue *sem_ready);
int mock_bluez_device_media_set_configuration(const char *device_path,
		const char *transport_path, const char *uuid, uint32_t codec_id,
		const void *configuration, size_t configuration_size,
		GAsyncQueue *sem_ready);
void mock_bluez_service_start(void);
void mock_bluez_service_stop(void);

int mock_upower_display_device_set_is_present(bool present);
int mock_upower_display_device_set_percentage(double percentage);
void mock_upower_service_start(void);
void mock_upower_service_stop(void);

void mock_sem_signal(GAsyncQueue *sem);
void mock_sem_wait(GAsyncQueue *sem);

GThread *mock_bt_dump_thread_new(int fd);
