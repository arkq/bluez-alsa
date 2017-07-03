/*
 * test-at.c
 * Copyright (c) 2016-2017 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <stdlib.h>
#include "inc/test.inc"
#include "../src/at.c"

int main(void) {

	struct bt_at at;

	assert(at_parse("ABC", &at) == -1);
	assert(at_parse("ATABC", &at) == -1);

	assert(at_parse("AT+COPS?", &at) == 0);
	assert(at.type == AT_CMD_TYPE_GET);
	assert(strcmp(at.command, "+COPS") == 0);
	assert(strcmp(at.value, "") == 0);

	assert(at_parse("AT+CLCK=\"SC\",0,\"1234\"", &at) == 0);
	assert(at.type == AT_CMD_TYPE_SET);
	assert(strcmp(at.command, "+CLCK") == 0);
	assert(strcmp(at.value, "\"SC\",0,\"1234\"") == 0);

	assert(at_parse("AT+COPS=?", &at) == 0);
	assert(at.type == AT_CMD_TYPE_TEST);
	assert(strcmp(at.command, "+COPS") == 0);
	assert(strcmp(at.value, "") == 0);

	return EXIT_SUCCESS;
}
