/*
 * test-at.c
 * Copyright (c) 2016-2017 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "inc/test.inc"
#include "../src/at.c"

int main(void) {

	struct bt_at at;
	enum hfp_ind indmap[20];
	enum hfp_ind indmap_ok[20];
	char buffer[256];

	/* invalid AT command lines */
	assert(at_parse("ABC\r", &at) == NULL);
	assert(at_parse("AT+CLCK?", &at) == NULL);
	assert(at_parse("\r\r", &at) == NULL);
	assert(at_parse("\r\nOK", &at) == NULL);

	/* parse AT plain command */
	assert(at_parse("AT+CLCC\r", &at) != NULL);
	assert(at.type == AT_TYPE_CMD);
	assert(strcmp(at.command, "+CLCC") == 0);
	assert(at.value == NULL);

	/* parse AT GET command */
	assert(at_parse("AT+COPS?\r", &at) != NULL);
	assert(at.type == AT_TYPE_CMD_GET);
	assert(strcmp(at.command, "+COPS") == 0);
	assert(at.value == NULL);

	/* parse AT SET command */
	assert(at_parse("AT+CLCK=\"SC\",0,\"1234\"\r", &at) != NULL);
	assert(at.type == AT_TYPE_CMD_SET);
	assert(strcmp(at.command, "+CLCK") == 0);
	assert(strcmp(at.value, "\"SC\",0,\"1234\"") == 0);

	/* parse AT TEST command */
	assert(at_parse("AT+COPS=?\r", &at) != NULL);
	assert(at.type == AT_TYPE_CMD_TEST);
	assert(strcmp(at.command, "+COPS") == 0);
	assert(at.value == NULL);

	/* parse response result code */
	assert(at_parse("\r\n+CIND:0,0,1,4,0,4,0\r\n", &at) != NULL);
	assert(at.type == AT_TYPE_RESP);
	assert(strcmp(at.command, "+CIND") == 0);
	assert(strcmp(at.value, "0,0,1,4,0,4,0") == 0);

	/* parse response result code with empty value */
	assert(at_parse("\r\n+CIND:\r\n", &at) != NULL);
	assert(at.type == AT_TYPE_RESP);
	assert(strcmp(at.command, "+CIND") == 0);
	assert(strcmp(at.value, "") == 0);

	/* parse unsolicited result code */
	assert(at_parse("\r\nRING\r\n", &at) != NULL);
	assert(at.type == AT_TYPE_RESP);
	assert(strcmp(at.command, "") == 0);
	assert(strcmp(at.value, "RING") == 0);

	/* case-insensitive command and case-sensitive value */
	assert(at_parse("aT+tEsT=VaLuE\r", &at) != NULL);
	assert(at.type == AT_TYPE_CMD_SET);
	assert(strcmp(at.command, "+TEST") == 0);
	assert(strcmp(at.value, "VaLuE") == 0);

	/* concatenated commands */
	const char *cmd = "\r\nOK\r\n\r\n+COPS:1\r\n";
	assert(at_parse(cmd, &at) == &cmd[6]);
	assert(at.type == AT_TYPE_RESP);
	assert(strcmp(at.command, "") == 0);
	assert(strcmp(at.value, "OK") == 0);

	/* build commands */
	assert(strcmp(at_build(buffer, AT_TYPE_RAW, "\r\nRING", NULL), "\r\nRING") == 0);
	assert(strcmp(at_build(buffer, AT_TYPE_CMD, "+CLCC", NULL), "AT+CLCC\r") == 0);
	assert(strcmp(at_build(buffer, AT_TYPE_CMD_GET, "+COPS", NULL), "AT+COPS?\r") == 0);
	assert(strcmp(at_build(buffer, AT_TYPE_CMD_SET, "+BCS", "1"), "AT+BCS=1\r") == 0);
	assert(strcmp(at_build(buffer, AT_TYPE_CMD_TEST, "+CIND", NULL), "AT+CIND=?\r") == 0);

	/* build response result code */
	assert(strcmp(at_build(buffer, AT_TYPE_RESP, "+CIND", ""), "\r\n+CIND:\r\n") == 0);

	/* build unsolicited result code */
	assert(strcmp(at_build(buffer, AT_TYPE_RESP, NULL, "OK"), "\r\nOK\r\n") == 0);

	/* parse +CIND response result code */
	assert(at_parse_cind("(\"call\",(0,1)),(\"xxx\",(0-3)),(\"signal\",(0-5))", indmap) == 0);
	memset(indmap_ok, HFP_IND_NULL, sizeof(indmap_ok));
	indmap_ok[0] = HFP_IND_CALL;
	indmap_ok[2] = HFP_IND_SIGNAL;
	assert(memcmp(indmap, indmap_ok, sizeof(indmap)) == 0);

	/* parse +CIND response with white-spaces */
	assert(at_parse_cind(" ( \"call\", ( 0, 1 ) ), ( \"signal\", ( 0-3 ) )", indmap) == 0);

	/* parse +CIND invalid response */
	assert(at_parse_cind("(incorrect,1-2)", indmap) == -1);

	return 0;
}
