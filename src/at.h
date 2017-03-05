/*
 * BlueALSA - io.c
 * Copyright (c) 2017 Juha Kuikka
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_AT_H_
#define BLUEALSA_AT_H_

enum at_cmd_type
{
	AT_CMD_TYPE_SET,
	AT_CMD_TYPE_GET,
	AT_CMD_TYPE_TEST,
};

#define AT_MAX_CMD_SIZE		16
#define AT_MAX_VALUE_SIZE	64

struct at_command
{
	enum at_cmd_type type;
	char command[AT_MAX_CMD_SIZE];
	char value[AT_MAX_VALUE_SIZE];
};

/* str gets modified */
int at_parse(char *str, struct at_command *cmd);

#endif
