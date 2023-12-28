/*
 * BlueALSA - cli.h
 * Copyright (c) 2016-2023 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_CLI_CLI_H_
#define BLUEALSA_CLI_CLI_H_

#include <stdbool.h>

#include <dbus/dbus.h>

#include "shared/dbus-client.h"
#include "shared/dbus-client-pcm.h"
#include "shared/log.h"

struct cli_config {

	/* initialized BlueALSA D-Bus context */
	struct ba_dbus_ctx dbus;

	bool quiet;
	bool verbose;

};

struct cli_command {
	const char *name;
	const char *description;
	int (*func)(int argc, char *argv[]);
};

typedef bool (*cli_get_ba_services_cb)(const char *name, void *data);

void cli_get_ba_services(cli_get_ba_services_cb func, void *data, DBusError *err);
bool cli_get_ba_pcm(const char *path, struct ba_pcm *pcm, DBusError *err);

bool cli_parse_value_on_off(const char *value, bool *out);

void cli_print_adapters(const struct ba_service_props *props);
void cli_print_profiles_and_codecs(const struct ba_service_props *props);
void cli_print_pcm_available_codecs(const struct ba_pcm *pcm, DBusError *err);
void cli_print_pcm_selected_codec(const struct ba_pcm *pcm);
void cli_print_pcm_soft_volume(const struct ba_pcm *pcm);
void cli_print_pcm_volume(const struct ba_pcm *pcm);
void cli_print_pcm_mute(const struct ba_pcm *pcm);
void cli_print_pcm_properties(const struct ba_pcm *pcm, DBusError *err);
void cli_print_usage(const char *format, ...);

#define cli_print_error(M, ...) \
	if (!config.quiet) { error(M, ##__VA_ARGS__); }
#define cmd_print_error(M, ...) \
	if (!config.quiet) { error("CMD \"%s\": " M, argv[0], ##__VA_ARGS__); }

/**
 * Global configuration. */
extern struct cli_config config;

#endif
