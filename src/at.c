/*
 * BlueALSA - at.c
 * Copyright (c) 2017 Juha Kuikka
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "at.h"

#include <ctype.h>
#include <string.h>

#include "shared/log.h"

/**
 * Parse AT command.
 *
 * @param str String to parse.
 * @param at Address of the AT structure, where the parsed information will
 *   be stored.
 * @return On success this function returns 0. Otherwise, -1 is returned. */
int at_parse(const char *str, struct bt_at *at) {

	memset(at, 0, sizeof(*at));

	/* skip initial whitespace */
	while (isspace(*str))
		str++;

	/* starts with AT? */
	if (strncasecmp(str, "AT", 2))
		return -1;

	/* can we find equals sign? */
	char *equal = strstr(str, "=");
	if (equal != NULL) {
		/* set (ATxxx=value) or test (ATxxx=?) */
		strncpy(at->command, str + 2, equal - str - 2);
		if (equal[1] == '?')
			at->type = AT_CMD_TYPE_TEST;
		else {

			char *end;
			char *value = at->value;

			at->type = AT_CMD_TYPE_SET;
			strncpy(value, equal + 1, sizeof(at->value) - 1);

			/* remove trailing whitespace */
			end = value + strlen(value) - 1;
			while (end >= value && isspace(*end))
				*end-- = '\0';

		}
	}
	else {
		/* get (ATxxx?) */
		at->type = AT_CMD_TYPE_GET;
		char *question = strstr(str, "?");
		if (question != NULL)
			strncpy(at->command, str + 2, question - str - 2);
		else
			return -1;
	}

	debug("AT command: type:%d, command:%s, value:%s", at->type, at->command, at->value);
	return 0;
}
