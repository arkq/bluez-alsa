/*
 * BlueALSA - at.c
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
 * Copyright (c) 2017 Juha Kuikka
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
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "shared/defs.h"
#include "shared/log.h"

/**
 * Convert AT type into a human-readable string.
 *
 * @param type AT message type.
 * @return Human-readable string. */
const char *at_type2str(enum bt_at_type type) {
	static const char *types[__AT_TYPE_MAX] = {
		[AT_TYPE_RAW] = "RAW",
		[AT_TYPE_CMD] = "CMD",
		[AT_TYPE_CMD_GET] = "GET",
		[AT_TYPE_CMD_SET] = "SET",
		[AT_TYPE_CMD_TEST] = "TEST",
		[AT_TYPE_RESP] = "RESP",
	};
	return types[type];
}

/**
 * Build AT message.
 *
 * @param buffer Address of the buffer, which should be big enough to contain
 *   AT message: len(command) + len(value) + 6 bytes.
 * @param buffer The size of the buffer.
 * @param type AT message type.
 * @param command AT command. If one wants to build unsolicited response code,
 *   this parameter should be set to NULL, otherwise AT command response will
 *   be build.
 * @param value AT command value or unsolicited response code.
 * @return Pointer to the destination buffer. */
char *at_build(char *buffer, size_t size, enum bt_at_type type,
		const char *command, const char *value) {
	switch (type) {
	case AT_TYPE_RAW:
		snprintf(buffer, size, "%s", command);
		break;
	case AT_TYPE_CMD:
		snprintf(buffer, size, "AT%s\r", command);
		break;
	case AT_TYPE_CMD_GET:
		snprintf(buffer, size, "AT%s?\r", command);
		break;
	case AT_TYPE_CMD_SET:
		snprintf(buffer, size, "AT%s=%s\r", command, value);
		break;
	case AT_TYPE_CMD_TEST:
		snprintf(buffer, size, "AT%s=?\r", command);
		break;
	case AT_TYPE_RESP:
		if (command != NULL)
			snprintf(buffer, size, "\r\n%s:%s\r\n", command, value);
		else
			snprintf(buffer, size, "\r\n%s\r\n", value);
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
			command[0] = command[sizeof(at->command) - 1] = '\0';
		}

		/* consume <LF> from the end of the response */
		if (feed[1] == '\n')
			feed++;

	}

	/* In the BT specification, all AT commands are in uppercase letters.
	 * However, if someone will not respect this "convention", we will make
	 * life easier by converting received command to all uppercase. */
	while (*command != '\0') {
		*command = toupper(*command);
		command++;
	}

	debug("AT message: %s: command=%s value=%s",
			at_type2str(at->type), at->command, at->value);
	return (char *)&feed[1];
}

/**
 * Parse AT +BIA SET command value.
 *
 * @param str Command value string.
 * @param state Array with the state to be updated.
 * @return On success this function returns 0, otherwise -1 is returned. */
int at_parse_set_bia(const char *str, bool state[__HFP_IND_MAX]) {

	enum hfp_ind ind = HFP_IND_NULL + 1;
	while (ind < __HFP_IND_MAX && *str != '\0') {
		switch (*str) {
		case '0':
			state[ind] = false;
			break;
		case '1':
			state[ind] = true;
			break;
		case ',':
			ind++;
		}
		str++;
	}

	return 0;
}

/**
 * Parse AT +CIND GET response.
 *
 * The maximal number of possible mappings is 20. This value is hard-coded,
 * and is defined by the HFP specification.
 *
 * Mapping stored in the map array is 0-based. However, indexes returned by
 * the +CIEV unsolicited result code are 1-based. Please, be aware of this
 * incompatibility.
 *
 * @param str Response string.
 * @param map Address where the mapping between the indicator index and the
 *   HFP indicator type will be stored.
 * @return On success this function returns 0, otherwise -1 is returned. */
int at_parse_get_cind(const char *str, enum hfp_ind map[20]) {

	static const struct {
		const char *str;
		enum hfp_ind ind;
	} mapping[] = {
		{ "service", HFP_IND_SERVICE },
		{ "call", HFP_IND_CALL },
		{ "callsetup", HFP_IND_CALLSETUP },
		{ "callheld", HFP_IND_CALLHELD },
		{ "signal", HFP_IND_SIGNAL },
		{ "roam", HFP_IND_ROAM },
		{ "battchg", HFP_IND_BATTCHG },
	};

	char ind[16];

	memset(map, HFP_IND_NULL, sizeof(*map) * 20);
	for (size_t i = 0; i < 20; i++) {
		if (sscanf(str, " ( \"%15[a-z]\" , ( %*[0-9,-] ) )", ind) != 1)
			return -1;
		for (size_t ii = 0; ii < ARRAYSIZE(mapping); ii++)
			if (strcmp(mapping[ii].str, ind) == 0) {
				map[i] = mapping[ii].ind;
				break;
			}
		if ((str = strstr(str, "),")) == NULL)
			break;
		str += 2;
	}

	return 0;
}

/**
 * Parse AT +CMER SET command value.
 *
 * @param str CMER command value string.
 * @param map Address where the CMER values will be stored.
 * @return On success this function returns 0, otherwise -1 is returned. */
int at_parse_set_cmer(const char *str, unsigned int map[5]) {

	char *tmp;

	for (size_t i = 0; i < 5; i++) {
		while (isspace(*str) || *str == ',')
			str++;
		int v = strtol(str, &tmp, 10);
		if (str == tmp)
			return *str == '\0' ? 0 : -1;
		map[i] = v;
		str = tmp;
	}

	return 0;
}

/**
 * Parse AT +XAPL SET command value.
 *
 * @param str XAPL command value string.
 * @param vendor Address where the vendor ID will be stored.
 * @param product Address where the product ID will be stored.
 * @param version Address where the version number will be stored.
 * @param features Address where the features bitmap will be stored.
 * @return On success this function returns 0, otherwise -1 is returned. */
int at_parse_set_xapl(const char *str, uint16_t *vendor, uint16_t *product,
		uint16_t *version, uint8_t *features) {

	unsigned int _vendor, _product, _version, _features;
	int n = 0;

	if (sscanf(str, "%x-%x-%x,%u%n", &_vendor, &_product, &_version, &_features, &n) != 4)
		return -1;
	if (str[n] != '\0')
		return -1;

	*vendor = _vendor;
	*product = _product;
	*version = _version;
	*features = _features;

	return 0;
}
