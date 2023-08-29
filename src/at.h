/*
 * BlueALSA - at.c
 * Copyright (c) 2016-2022 Arkadiusz Bokowy
 * Copyright (c) 2017 Juha Kuikka
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#pragma once
#ifndef BLUEALSA_AT_H_
#define BLUEALSA_AT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hfp.h"

enum bt_at_type {
	AT_TYPE_RAW,
	AT_TYPE_CMD,
	AT_TYPE_CMD_GET,
	AT_TYPE_CMD_SET,
	AT_TYPE_CMD_TEST,
	AT_TYPE_RESP,
	__AT_TYPE_MAX
};

struct bt_at {
	enum bt_at_type type;
	char command[256];
	char *value;
};

const char *at_type2str(enum bt_at_type type);

char *at_build(char *buffer, size_t size, enum bt_at_type type,
		const char *command, const char *value);

char *at_parse(const char *str, struct bt_at *at);
int at_parse_set_bia(const char *str, bool state[__HFP_IND_MAX]);
int at_parse_get_cind(const char *str, enum hfp_ind map[20]);
int at_parse_set_cmer(const char *str, unsigned int map[5]);
int at_parse_set_xapl(const char *str, uint16_t *vendor, uint16_t *product,
		uint16_t *version, uint8_t *features);

#endif
