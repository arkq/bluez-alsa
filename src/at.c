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

#include "at.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "shared/log.h"


/**
 * Build AT message.
 *
 * @param buffer Address of the buffer, which shall be big enough to contain
 *   AT message: len(command) + len(value) + 6 bytes.
 * @param type AT message type.
 * @param command AT command. If one wants to build unsolicited response code,
 *   this parameter should be set to NULL, otherwise AT command response will
 *   be build.
 * @param value AT command value or unsolicited response code.
 * @return Pointer to the destination buffer. */
char *at_build(char *buffer, enum bt_at_type type, const char *command,
		const char *value) {
	switch (type) {
	case AT_TYPE_CMD:
		sprintf(buffer, "AT%s\r", command);
		break;
	case AT_TYPE_CMD_GET:
		sprintf(buffer, "AT%s?\r", command);
		break;
	case AT_TYPE_CMD_SET:
		sprintf(buffer, "AT%s=%s\r", command, value);
		break;
	case AT_TYPE_CMD_TEST:
		sprintf(buffer, "AT%s=?\r", command);
		break;
	case AT_TYPE_RESP:
		if (command != NULL)
			sprintf(buffer, "\r\n%s:%s\r\n", command, value);
		else
			sprintf(buffer, "\r\n%s\r\n", value);
		break;
	case __AT_TYPE_MAX:
		break;
	}
	return buffer;
}

/**
 * Parse AT message.
 *
 * @param str String to parse.
 * @param at Address of the AT structure, where the parsed information will
 *   be stored.
 * @return On success this function returns a pointer to the next message
 *   within the input string. If the input string contains only one message,
 *   returned value will point to the end null byte. On error, this function
 *   returns NULL. */
char *at_parse(const char *str, struct bt_at *at) {

	char *command = at->command;
	bool is_command = false;
	const char *feed;
	char *tmp;

	/* locate <CR> character, which indicates end of message */
	if ((feed = strchr(str, '\r')) == NULL)
		return NULL;

	/* consume empty message */
	if (feed == str)
		return at_parse(feed + 1, at);

	/* check whether we are parsing AT command */
	if (strncasecmp(str, "AT", 2) == 0) {
		is_command = true;
		str += 2;
	}
	else {
		/* response starts with <LF> sequence */
		if (str[0] != '\n')
			return NULL;
		str += 1;
	}

	strncpy(command, str, sizeof(at->command) - 1);
	at->value = NULL;

	/* check everything twice (we don't want to be hacked) */
	if ((size_t)(feed - str) < sizeof(at->command))
		command[feed - str] = '\0';

	if (is_command) {

		/* determine command type */
		if ((tmp = strchr(command, '=')) != NULL) {
			if (tmp[1] == '?')
				at->type = AT_TYPE_CMD_TEST;
			else {
				at->type = AT_TYPE_CMD_SET;
				at->value = tmp + 1;
			}
		}
		else if ((tmp = strchr(command, '?')) != NULL)
			at->type = AT_TYPE_CMD_GET;
		else
			at->type = AT_TYPE_CMD;

		if (tmp != NULL)
			*tmp = '\0';

	}
	else {

		at->type = AT_TYPE_RESP;

		if ((tmp = strchr(command, ':')) == NULL)
			/* provide support for GSM standard */
			tmp = strchr(command, '=');

		if (tmp != NULL) {
			at->value = tmp + 1;
			*tmp = '\0';
		}
		else {
			/* unsolicited (with empty command) result code */
			at->value = memmove(command + 1, command, sizeof(at->command) - 1);
			command[0] = command[sizeof(at->command)] = '\0';
		}

	}

	/* In the BT specification, all AT commands are in uppercase letters.
	 * However, if someone will not respect this "convention", we will make
	 * life easier by converting received command to all uppercase. */
	while (*command != '\0') {
		*command = toupper(*command);
		command++;
	}

	debug("AT message: %s: command:%s, value:%s", at_type2str(at->type), at->command, at->value);
	return (char *)&feed[1];
}

/**
 * Convert AT type into a human-readable string.
 *
 * @param type AT message type.
 * @return Human-readable string. */
const char *at_type2str(enum bt_at_type type) {
	static const char *types[__AT_TYPE_MAX] = {
		[AT_TYPE_CMD] = "CMD",
		[AT_TYPE_CMD_GET] = "GET",
		[AT_TYPE_CMD_SET] = "SET",
		[AT_TYPE_CMD_TEST] = "TEST",
		[AT_TYPE_RESP] = "RESP",
	};
	return types[type];
}
