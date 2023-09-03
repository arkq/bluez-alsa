/*
 * mock.h
 * Copyright (c) 2016-2023 Arkadiusz Bokowy
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

#include <gio/gio.h>
#include <glib.h>

#define MOCK_ADAPTER_ID 0
#define MOCK_DEVICE_1 "12:34:56:78:9A:BC"
#define MOCK_DEVICE_2 "23:45:67:89:AB:CD"

#define MOCK_BLUEZ_ADAPTER_PATH "/org/bluez/hci0"
#define MOCK_BLUEZ_DEVICE_PATH_1 MOCK_BLUEZ_ADAPTER_PATH "/dev_12_34_56_78_9A_BC"
#define MOCK_BLUEZ_DEVICE_PATH_2 MOCK_BLUEZ_ADAPTER_PATH "/dev_23_45_67_89_AB_CD"
#define MOCK_BLUEZ_MEDIA_TRANSPORT_PATH_1 MOCK_BLUEZ_DEVICE_PATH_1 "/fdX"
#define MOCK_BLUEZ_MEDIA_TRANSPORT_PATH_2 MOCK_BLUEZ_DEVICE_PATH_2 "/fdX"
#define MOCK_BLUEZ_MIDI_PATH_1 MOCK_BLUEZ_DEVICE_PATH_1 "/midi"
#define MOCK_BLUEZ_SCO_PATH_1 MOCK_BLUEZ_DEVICE_PATH_1 "/sco"
#define MOCK_BLUEZ_SCO_PATH_2 MOCK_BLUEZ_DEVICE_PATH_2 "/sco"

extern struct ba_adapter *mock_adapter;

extern GAsyncQueue *mock_sem_timeout;
extern GAsyncQueue *mock_sem_quit;

extern bool mock_dump_output;
extern int mock_fuzzing_ms;

int mock_bluez_device_name_mapping_add(const char *mapping);
void mock_bluealsa_dbus_name_acquired(GDBusConnection *conn, const char *name, void *userdata);
void mock_bluez_dbus_name_acquired(GDBusConnection *conn, const char *name, void *userdata);

void mock_sem_signal(GAsyncQueue *sem);
void mock_sem_wait(GAsyncQueue *sem);
