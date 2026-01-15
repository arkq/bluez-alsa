/*
 * BlueALSA - bt-gatt.h
 * SPDX-FileCopyrightText: 2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BLUEALSA_BT_GATT_H_
#define BLUEALSA_BT_GATT_H_

#include <stdbool.h>

#include <gio/gio.h>
#include <glib-object.h>
#include <glib.h>

#include "ba-adapter.h"

#define BLUETOOTH_TYPE_GATT_APPLICATION (bluetooth_gatt_application_get_type())
G_DECLARE_FINAL_TYPE(BluetoothGATTApplication, bluetooth_gatt_application,
		BLUETOOTH, GATT_APPLICATION, GObject)
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(BluetoothGATTApplication, g_object_unref)

#define BLUETOOTH_TYPE_GATT_SERVICE (bluetooth_gatt_service_get_type())
G_DECLARE_FINAL_TYPE(BluetoothGATTService, bluetooth_gatt_service,
		BLUETOOTH, GATT_SERVICE, GObject)
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(BluetoothGATTService, g_object_unref)

#define BLUETOOTH_TYPE_GATT_CHARACTERISTIC (bluetooth_gatt_characteristic_get_type())
G_DECLARE_FINAL_TYPE(BluetoothGATTCharacteristic, bluetooth_gatt_characteristic,
		BLUETOOTH, GATT_CHARACTERISTIC, GObject)
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(BluetoothGATTCharacteristic, g_object_unref)

BluetoothGATTApplication * bluetooth_gatt_application_new(
		const char * path);
GDBusObjectManagerServer * bluetooth_gatt_application_get_object_manager_server(
		BluetoothGATTApplication * app);

void bluetooth_gatt_application_add_service(
		BluetoothGATTApplication * app,
		BluetoothGATTService * srv);
void bluetooth_gatt_application_add_service_characteristic(
		BluetoothGATTApplication * app,
		BluetoothGATTService * srv,
		BluetoothGATTCharacteristic * chr);
void bluetooth_gatt_application_set_connection(
		BluetoothGATTApplication * app,
		GDBusConnection * conn);

void bluetooth_gatt_application_register(
		BluetoothGATTApplication * app,
		struct ba_adapter * adapter,
		GAsyncReadyCallback callback,
		void * userdata);
bool bluetooth_gatt_application_register_finish(
		BluetoothGATTApplication * app,
		GAsyncResult * result,
		GError ** error);

BluetoothGATTService * bluetooth_gatt_service_new(
		const char * path,
		const char * uuid,
		bool primary);

typedef bool (*BluetoothGATTCharacteristicCallback)(
		BluetoothGATTCharacteristic * chr,
		GDBusMethodInvocation * inv,
		void * userdata);

BluetoothGATTCharacteristic * bluetooth_gatt_characteristic_new(
		const char * path,
		const char * uuid);

void bluetooth_gatt_characteristic_set_flags(
		BluetoothGATTCharacteristic * chr,
		const char * const * flags);
void bluetooth_gatt_characteristic_set_read_callback(
		BluetoothGATTCharacteristic * chr,
		BluetoothGATTCharacteristicCallback callback,
		void * userdata);
void bluetooth_gatt_characteristic_set_acquire_notify_callback(
		BluetoothGATTCharacteristic * chr,
		BluetoothGATTCharacteristicCallback callback,
		void * userdata);
void bluetooth_gatt_characteristic_set_acquire_write_callback(
		BluetoothGATTCharacteristic * chr,
		BluetoothGATTCharacteristicCallback callback,
		void * userdata);

#endif
