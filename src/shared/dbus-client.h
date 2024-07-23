/*
 * BlueALSA - dbus-client.h
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_SHARED_DBUSCLIENT_H_
#define BLUEALSA_SHARED_DBUSCLIENT_H_

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <poll.h>
#include <stddef.h>

#include <bluetooth/bluetooth.h> /* IWYU pragma: keep */
#include <bluetooth/hci.h>
#include <dbus/dbus.h>

#ifndef DBUS_INTERFACE_OBJECT_MANAGER
# define DBUS_INTERFACE_OBJECT_MANAGER "org.freedesktop.DBus.ObjectManager"
#endif

#define BLUEALSA_SERVICE           "org.bluealsa"
#define BLUEALSA_INTERFACE_MANAGER "org.bluealsa.Manager1"
#define BLUEALSA_INTERFACE_PCM     "org.bluealsa.PCM1"
#define BLUEALSA_INTERFACE_RFCOMM  "org.bluealsa.RFCOMM1"

/**
 * Connection context. */
struct ba_dbus_ctx {
	/* private D-Bus connection */
	DBusConnection *conn;
	/* registered watches */
	DBusWatch **watches;
	size_t watches_len;
	/* registered matches */
	char **matches;
	size_t matches_len;
	/* BlueALSA service name */
	char ba_service[32];
};

dbus_bool_t ba_dbus_connection_ctx_init(
		struct ba_dbus_ctx *ctx,
		const char *ba_service_name,
		DBusError *error);

void ba_dbus_connection_ctx_free(
		struct ba_dbus_ctx *ctx);

dbus_bool_t ba_dbus_connection_signal_match_add(
		struct ba_dbus_ctx *ctx,
		const char *sender,
		const char *path,
		const char *iface,
		const char *member,
		const char *extra);

dbus_bool_t ba_dbus_connection_signal_match_clean(
		struct ba_dbus_ctx *ctx);

dbus_bool_t ba_dbus_connection_dispatch(
		struct ba_dbus_ctx *ctx);

dbus_bool_t ba_dbus_connection_poll_fds(
		struct ba_dbus_ctx *ctx,
		struct pollfd *fds,
		nfds_t *nfds);

dbus_bool_t ba_dbus_connection_poll_dispatch(
		struct ba_dbus_ctx *ctx,
		struct pollfd *fds,
		nfds_t nfds);

dbus_bool_t ba_dbus_props_get_all(
		struct ba_dbus_ctx *ctx,
		const char *path,
		const char *interface,
		DBusError *error,
		dbus_bool_t (*cb)(const char *key, DBusMessageIter *val, void *data, DBusError *err),
		void *userdata);

/**
 * BlueALSA service property object. */
struct ba_service_props {
	/* service version */
	char version[32];
	/* currently used HCI adapters */
	char adapters[HCI_MAX_DEV][8];
	size_t adapters_len;
	/* currently used Bluetooth profiles */
	char **profiles;
	size_t profiles_len;
	/* currently used audio codecs */
	char **codecs;
	size_t codecs_len;
};

dbus_bool_t ba_dbus_service_props_get(
		struct ba_dbus_ctx *ctx,
		struct ba_service_props *props,
		DBusError *error);

void ba_dbus_service_props_free(
		struct ba_service_props *props);

dbus_bool_t dbus_message_iter_array_get_strings(
		DBusMessageIter *iter,
		DBusError *error,
		const char **strings,
		size_t *length);

dbus_bool_t dbus_message_iter_dict(
		DBusMessageIter *iter,
		DBusError *error,
		dbus_bool_t (*cb)(const char *key, DBusMessageIter *val, void *data, DBusError *err),
		void *userdata);

dbus_bool_t dbus_message_iter_dict_append_basic(
		DBusMessageIter *iter,
		const char *key,
		int value_type,
		const void *value);

int dbus_error_to_errno(
		const DBusError *error);

#endif
