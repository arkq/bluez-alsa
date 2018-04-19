/*
 * BlueALSA - at.c
 * Copyright (c) 2016-2017 Arkadiusz Bokowy
 *               2017 Juha Kuikka
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_AT_H_
#define BLUEALSA_AT_H_

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

char *at_build(char *buffer, enum bt_at_type type, const char *command,
		const char *value);
char *at_parse(const char *str, struct bt_at *at);
int at_parse_cind(const char *str, enum hfp_ind map[20]);
const char *at_type2str(enum bt_at_type type);

#endif
