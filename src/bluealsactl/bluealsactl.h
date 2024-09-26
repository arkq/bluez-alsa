/*
 * BlueALSA - bluealsactl/bluealsactl.h
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSACTL_BLUEALSACTL_H_
#define BLUEALSACTL_BLUEALSACTL_H_

#include <stdbool.h>

#include <dbus/dbus.h>

#include "shared/dbus-client.h"
#include "shared/dbus-client-pcm.h"
#include "shared/log.h"

struct bactl_config {

	/* initialized BlueALSA D-Bus context */
	struct ba_dbus_ctx dbus;

	bool quiet;
	unsigned int verbose;

};

struct bactl_command {
	const char *name;
	const char *description;
	int (*func)(int argc, char *argv[]);
};

typedef bool (*bactl_get_ba_services_cb)(const char *name, void *data);

void bactl_get_ba_services(bactl_get_ba_services_cb func, void *data, DBusError *err);
bool bactl_get_ba_pcm(const char *path, struct ba_pcm *pcm, DBusError *err);

bool bactl_parse_common_options(int opt);
bool bactl_parse_value_on_off(const char *value, bool *out);

void bactl_print_adapters(const struct ba_service_props *props);
void bactl_print_profiles_and_codecs(const struct ba_service_props *props);
void bactl_print_pcm_available_codecs(const struct ba_pcm *pcm, DBusError *err);
void bactl_print_pcm_selected_codec(const struct ba_pcm *pcm);
void bactl_print_pcm_delay(const struct ba_pcm *pcm);
void bactl_print_pcm_client_delay(const struct ba_pcm *pcm);
void bactl_print_pcm_soft_volume(const struct ba_pcm *pcm);
void bactl_print_pcm_volume(const struct ba_pcm *pcm);
void bactl_print_pcm_mute(const struct ba_pcm *pcm);
void bactl_print_pcm_properties(const struct ba_pcm *pcm, DBusError *err);
void bactl_print_usage(const char *format, ...);

#define bactl_print_error(M, ...) \
	if (!config.quiet) { error(M, ##__VA_ARGS__); }
#define cmd_print_error(M, ...) \
	if (!config.quiet) { error("CMD \"%s\": " M, argv[0], ##__VA_ARGS__); }

/**
 * Global configuration. */
extern struct bactl_config config;

#endif
