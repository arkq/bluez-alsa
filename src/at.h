/*
 * BlueALSA - at.c
 * Copyright (c) 2017 Juha Kuikka
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_AT_H_
#define BLUEALSA_AT_H_

enum bt_at_type {
	AT_CMD_TYPE_GET,
	AT_CMD_TYPE_SET,
	AT_CMD_TYPE_TEST,
};

struct bt_at {
	enum bt_at_type type;
	char command[16];
	char value[64];
};

int at_parse(const char *str, struct bt_at *at);

#endif
