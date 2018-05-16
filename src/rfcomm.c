/*
 * BlueALSA - rfcomm.c
 * Copyright (c) 2016-2018 Arkadiusz Bokowy
 *               2017 Juha Kuikka
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "rfcomm.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bluealsa.h"
#include "ctl.h"
#include "utils.h"
#include "shared/log.h"


/**
 * Structure used for buffered reading from the RFCOMM. */
struct at_reader {
	struct bt_at at;
	char buffer[256];
	/* pointer to the next message within the buffer */
	char *next;
};

/**
 * Read AT message.
 *
 * Upon error it is required to set the next pointer of the reader structure
 * to NULL. Otherwise, this function might fail indefinitely.
 *
 * @param fd RFCOMM socket file descriptor.
 * @param reader Pointer to initialized reader structure.
 * @return On success this function returns 0. Otherwise, -1 is returned and
 *   errno is set to indicate the error. */
static int rfcomm_read_at(int fd, struct at_reader *reader) {

	char *buffer = reader->buffer;
	char *msg = reader->next;
	char *tmp;

	/* In case of reading more than one message from the RFCOMM, we have to
	 * parse all of them before we can read from the socket once more. */
	if (msg == NULL) {

		ssize_t len;

retry:
		if ((len = read(fd, buffer, sizeof(reader->buffer))) == -1) {
			if (errno == EINTR)
				goto retry;
			return -1;
		}

		buffer[len] = '\0';
		msg = buffer;
	}

	/* parse AT message received from the RFCOMM */
	if ((tmp = at_parse(msg, &reader->at)) == NULL) {
		reader->next = msg;
		errno = EBADMSG;
		return -1;
	}

	reader->next = tmp[0] != '\0' ? tmp : NULL;
	return 0;
}

/**
 * Write AT message.
 *
 * @param fd RFCOMM socket file descriptor.
 * @param type Type of the AT message.
 * @param command AT command or response code.
 * @param value AT value or NULL if not applicable.
 * @return On success this function returns 0. Otherwise, -1 is returned and
 *   errno is set to indicate the error. */
static int rfcomm_write_at(int fd, enum bt_at_type type, const char *command,
		const char *value) {

	char msg[256];
	size_t len;

	debug("Sending AT message: %s: command:%s, value:%s",
			at_type2str(type), command, value);

	at_build(msg, type, command, value);
	len = strlen(msg);

retry:
	if (write(fd, msg, len) == -1) {
		if (errno == EINTR)
			goto retry;
		return -1;
	}

	return 0;
}

/**
 * HFP set state wrapper for debugging purposes. */
static void rfcomm_set_hfp_state(struct rfcomm_conn *c, enum hfp_state state) {
	debug("HFP state transition: %d -> %d", c->state, state);
	c->state = state;
}

/**
 * Handle AT command response code. */
static int rfcomm_handler_resp_ok_cb(struct rfcomm_conn *c, const struct bt_at *at) {

	/* advance service level connection state */
	if (strcmp(at->value, "OK") == 0) {
		rfcomm_set_hfp_state(c, c->state + 1);
		return 0;
	}
	/* indicate caller that error has occurred */
	if (strcmp(at->value, "ERROR") == 0) {
		errno = ENOTSUP;
		return -1;
	}

	return 0;
}

/**
 * TEST: Standard indicator update AT command */
static int rfcomm_handler_cind_test_cb(struct rfcomm_conn *c, const struct bt_at *at) {
	(void)at;
	const int fd = c->t->bt_fd;

	if (rfcomm_write_at(fd, AT_TYPE_RESP, "+CIND",
				"(\"call\",(0,1))"
				",(\"callsetup\",(0-3))"
				",(\"service\",(0-1))"
				",(\"signal\",(0-5))"
				",(\"roam\",(0-1))"
				",(\"battchg\",(0-5))"
				",(\"callheld\",(0-2))"
			) == -1)
		return -1;
	if (rfcomm_write_at(fd, AT_TYPE_RESP, NULL, "OK") == -1)
		return -1;

	if (c->state < HFP_SLC_CIND_TEST_OK)
		rfcomm_set_hfp_state(c, HFP_SLC_CIND_TEST_OK);

	return 0;
}

/**
 * GET: Standard indicator update AT command */
static int rfcomm_handler_cind_get_cb(struct rfcomm_conn *c, const struct bt_at *at) {
	(void)at;
	const int fd = c->t->bt_fd;

	if (rfcomm_write_at(fd, AT_TYPE_RESP, "+CIND", "0,0,0,0,0,0,0") == -1)
		return -1;
	if (rfcomm_write_at(fd, AT_TYPE_RESP, NULL, "OK") == -1)
		return -1;

	if (c->state < HFP_SLC_CIND_GET_OK)
		rfcomm_set_hfp_state(c, HFP_SLC_CIND_GET_OK);

	return 0;
}

/**
 * RESP: Standard indicator update AT command */
static int rfcomm_handler_cind_resp_test_cb(struct rfcomm_conn *c, const struct bt_at *at) {
	/* parse response for the +CIND TEST command */
	if (at_parse_cind(at->value, c->hfp_ind_map) == -1)
		warn("Couldn't parse AG indicators");
	if (c->state < HFP_SLC_CIND_TEST)
		rfcomm_set_hfp_state(c, HFP_SLC_CIND_TEST);
	return 0;
}

/**
 * RESP: Standard indicator update AT command */
static int rfcomm_handler_cind_resp_get_cb(struct rfcomm_conn *c, const struct bt_at *at) {

	struct ba_transport * const t = c->t;
	char *tmp = at->value;
	size_t i;

	/* parse response for the +CIND GET command */
	for (i = 0; i < sizeof(c->hfp_ind_map) / sizeof(*c->hfp_ind_map); i++) {
		t->rfcomm.hfp_inds[c->hfp_ind_map[i]] = atoi(tmp);
		if (c->hfp_ind_map[i] == HFP_IND_BATTCHG)
			device_set_battery_level(t->device, atoi(tmp) * 100 / 5);
		if ((tmp = strchr(tmp, ',')) == NULL)
			break;
		tmp += 1;
	}

	if (c->state < HFP_SLC_CIND_GET)
		rfcomm_set_hfp_state(c, HFP_SLC_CIND_GET);

	return 0;
}

/**
 * SET: Standard event reporting activation/deactivation AT command */
static int rfcomm_handler_cmer_set_cb(struct rfcomm_conn *c, const struct bt_at *at) {
	(void)at;
	const int fd = c->t->bt_fd;

	if (rfcomm_write_at(fd, AT_TYPE_RESP, NULL, "OK") == -1)
		return -1;

	if (c->state < HFP_SLC_CMER_SET_OK)
		rfcomm_set_hfp_state(c, HFP_SLC_CMER_SET_OK);

	return 0;
}

/**
 * RESP: Standard indicator events reporting unsolicited result code */
static int rfcomm_handler_ciev_resp_cb(struct rfcomm_conn *c, const struct bt_at *at) {

	struct ba_transport * const t = c->t;
	unsigned int index;
	unsigned int value;

	if (sscanf(at->value, "%u,%u", &index, &value) == 2) {
		t->rfcomm.hfp_inds[c->hfp_ind_map[index - 1]] = value;
		switch (c->hfp_ind_map[index - 1]) {
		case HFP_IND_CALL:
		case HFP_IND_CALLSETUP:
			transport_send_signal(t->rfcomm.sco, TRANSPORT_PCM_OPEN);
			break;
		case HFP_IND_BATTCHG:
			device_set_battery_level(t->device, value * 100 / 5);
			break;
		default:
			break;
		}
	}

	return 0;
}

/**
 * SET: Bluetooth Indicators Activation */
static int rfcomm_handler_bia_set_cb(struct rfcomm_conn *c, const struct bt_at *at) {
	(void)at;
	/* We are not sending any indicators to the HF, however support for the
	 * +BIA command is mandatory for the AG, so acknowledge the message. */
	if (rfcomm_write_at(c->t->bt_fd, AT_TYPE_RESP, NULL, "OK") == -1)
		return -1;
	return 0;
}

/**
 * SET: Bluetooth Retrieve Supported Features */
static int rfcomm_handler_brsf_set_cb(struct rfcomm_conn *c, const struct bt_at *at) {

	struct ba_transport * const t = c->t;
	const int fd = t->bt_fd;
	char tmp[16];

	t->rfcomm.hfp_features = atoi(at->value);

	/* Codec negotiation is not supported in the HF, hence no
	 * wideband audio support. AT+BAC will not be sent. */
	if (!(t->rfcomm.hfp_features & HFP_HF_FEAT_CODEC))
		t->rfcomm.sco->codec = HFP_CODEC_CVSD;

	sprintf(tmp, "%u", config.hfp.features_rfcomm_ag);
	if (rfcomm_write_at(fd, AT_TYPE_RESP, "+BRSF", tmp) == -1)
		return -1;
	if (rfcomm_write_at(fd, AT_TYPE_RESP, NULL, "OK") == -1)
		return -1;

	if (c->state < HFP_SLC_BRSF_SET_OK)
		rfcomm_set_hfp_state(c, HFP_SLC_BRSF_SET_OK);

	return 0;
}

/**
 * RESP: Bluetooth Retrieve Supported Features */
static int rfcomm_handler_brsf_resp_cb(struct rfcomm_conn *c, const struct bt_at *at) {

	struct ba_transport * const t = c->t;
	t->rfcomm.hfp_features = atoi(at->value);

	/* codec negotiation is not supported in the AG */
	if (!(t->rfcomm.hfp_features & HFP_AG_FEAT_CODEC))
		t->rfcomm.sco->codec = HFP_CODEC_CVSD;

	if (c->state < HFP_SLC_BRSF_SET)
		rfcomm_set_hfp_state(c, HFP_SLC_BRSF_SET);

	return 0;
}

/**
 * SET: Gain of Microphone */
static int rfcomm_handler_vgm_set_cb(struct rfcomm_conn *c, const struct bt_at *at) {
	struct ba_transport * const t = c->t;
	const int fd = t->bt_fd;

	t->rfcomm.sco->sco.mic_gain = c->mic_gain = atoi(at->value);
	if (rfcomm_write_at(fd, AT_TYPE_RESP, NULL, "OK") == -1)
		return -1;

	bluealsa_ctl_event(BA_EVENT_UPDATE_VOLUME);
	return 0;
}

/**
 * SET: Gain of Speaker */
static int rfcomm_handler_vgs_set_cb(struct rfcomm_conn *c, const struct bt_at *at) {
	struct ba_transport * const t = c->t;
	const int fd = t->bt_fd;

	t->rfcomm.sco->sco.spk_gain = c->spk_gain = atoi(at->value);
	if (rfcomm_write_at(fd, AT_TYPE_RESP, NULL, "OK") == -1)
		return -1;

	bluealsa_ctl_event(BA_EVENT_UPDATE_VOLUME);
	return 0;
}

/**
 * SET: Bluetooth Response and Hold Feature */
static int rfcomm_handler_btrh_get_cb(struct rfcomm_conn *c, const struct bt_at *at) {
	(void)at;
	/* Currently, we are not supporting Respond & Hold feature, so just
	 * acknowledge this GET request without reporting +BTRH status. */
	if (rfcomm_write_at(c->t->bt_fd, AT_TYPE_RESP, NULL, "OK") == -1)
		return -1;
	return 0;
}

/**
 * SET: Bluetooth Codec Selection */
static int rfcomm_handler_bcs_set_cb(struct rfcomm_conn *c, const struct bt_at *at) {

	struct ba_transport * const t = c->t;
	const int fd = t->bt_fd;

	if (t->rfcomm.sco->codec != atoi(at->value)) {
		warn("Codec not acknowledged: %d != %s", t->rfcomm.sco->codec, at->value);
		if (rfcomm_write_at(fd, AT_TYPE_RESP, NULL, "ERROR") == -1)
			return -1;
		return 0;
	}

	if (rfcomm_write_at(fd, AT_TYPE_RESP, NULL, "OK") == -1)
		return -1;

	if (c->state < HFP_CC_BCS_SET_OK)
		rfcomm_set_hfp_state(c, HFP_CC_BCS_SET_OK);

	return 0;
}

static int rfcomm_handler_resp_bcs_ok_cb(struct rfcomm_conn *c, const struct bt_at *at) {
	if (rfcomm_handler_resp_ok_cb(c, at) != 0)
		return -1;
	/* When codec selection is completed, notify connected clients, that
	 * transport has been changed. Note, that this event might be emitted
	 * for an active transport - codec switching. */
	bluealsa_ctl_event(BA_EVENT_TRANSPORT_CHANGED);
	return 0;
}

/**
 * RESP: Bluetooth Codec Selection */
static int rfcomm_handler_bcs_resp_cb(struct rfcomm_conn *c, const struct bt_at *at) {

	static const struct rfcomm_handler handler = {
		AT_TYPE_RESP, "", rfcomm_handler_resp_bcs_ok_cb };
	struct ba_transport * const t = c->t;
	const int fd = t->bt_fd;

	t->rfcomm.sco->codec = atoi(at->value);
	if (rfcomm_write_at(fd, AT_TYPE_CMD_SET, "+BCS", at->value) == -1)
		return -1;

	c->handler = &handler;

	if (c->state < HFP_CC_BCS_SET)
		rfcomm_set_hfp_state(c, HFP_CC_BCS_SET);

	return 0;
}

/**
 * SET: Bluetooth Available Codecs */
static int rfcomm_handler_bac_set_cb(struct rfcomm_conn *c, const struct bt_at *at) {
	(void)at;
	const int fd = c->t->bt_fd;

	/* In case some headsets send BAC even if we don't advertise
	 * support for it. In such case, just OK and ignore. */

	if (rfcomm_write_at(fd, AT_TYPE_RESP, NULL, "OK") == -1)
		return -1;

	if (c->state < HFP_SLC_BAC_SET_OK)
		rfcomm_set_hfp_state(c, HFP_SLC_BAC_SET_OK);

	return 0;
}

/**
 * SET: Apple Ext: Report a headset state change */
static int rfcomm_handler_iphoneaccev_set_cb(struct rfcomm_conn *c, const struct bt_at *at) {

	struct ba_transport * const t = c->t;
	struct ba_device * const d = t->device;
	const int fd = t->bt_fd;

	char *ptr = at->value;
	size_t count = atoi(strsep(&ptr, ","));
	char tmp;

	while (count-- && ptr != NULL)
		switch (tmp = *strsep(&ptr, ",")) {
		case '1':
			if (ptr != NULL)
				device_set_battery_level(d, atoi(strsep(&ptr, ",")) * 100 / 9);
			break;
		case '2':
			if (ptr != NULL)
				d->xapl.accev_docked = atoi(strsep(&ptr, ","));
			break;
		default:
			warn("Unsupported IPHONEACCEV key: %c", tmp);
			strsep(&ptr, ",");
		}

	if (rfcomm_write_at(fd, AT_TYPE_RESP, NULL, "OK") == -1)
		return -1;
	return 0;
}

/**
 * SET: Apple Ext: Enable custom AT commands from an accessory */
static int rfcomm_handler_xapl_set_cb(struct rfcomm_conn *c, const struct bt_at *at) {

	struct ba_transport * const t = c->t;
	struct ba_device * const d = t->device;
	const int fd = t->bt_fd;

	const char *resp = "+XAPL=BlueALSA,0";
	unsigned int vendor, product;
	unsigned int version, features;

	if (sscanf(at->value, "%x-%x-%u,%u", &vendor, &product, &version, &features) == 4) {
		d->xapl.vendor_id = vendor;
		d->xapl.product_id = product;
		d->xapl.version = version;
		d->xapl.features = features;
	}
	else {
		warn("Invalid XAPL value: %s", at->value);
		resp = "ERROR";
	}

	if (rfcomm_write_at(fd, AT_TYPE_RESP, NULL, resp) == -1)
		return -1;
	return 0;
}

static const struct rfcomm_handler rfcomm_handler_resp_ok = {
	AT_TYPE_RESP, "", rfcomm_handler_resp_ok_cb };
static const struct rfcomm_handler rfcomm_handler_cind_test = {
	AT_TYPE_CMD_TEST, "+CIND", rfcomm_handler_cind_test_cb };
static const struct rfcomm_handler rfcomm_handler_cind_get = {
	AT_TYPE_CMD_GET, "+CIND", rfcomm_handler_cind_get_cb };
static const struct rfcomm_handler rfcomm_handler_cind_resp_test = {
	AT_TYPE_RESP, "+CIND", rfcomm_handler_cind_resp_test_cb };
static const struct rfcomm_handler rfcomm_handler_cind_resp_get = {
	AT_TYPE_RESP, "+CIND", rfcomm_handler_cind_resp_get_cb };
static const struct rfcomm_handler rfcomm_handler_cmer_set = {
	AT_TYPE_CMD_SET, "+CMER", rfcomm_handler_cmer_set_cb };
static const struct rfcomm_handler rfcomm_handler_ciev_resp = {
	AT_TYPE_RESP, "+CIEV", rfcomm_handler_ciev_resp_cb };
static const struct rfcomm_handler rfcomm_handler_bia_set = {
	AT_TYPE_CMD_SET, "+BIA", rfcomm_handler_bia_set_cb };
static const struct rfcomm_handler rfcomm_handler_brsf_set = {
	AT_TYPE_CMD_SET, "+BRSF", rfcomm_handler_brsf_set_cb };
static const struct rfcomm_handler rfcomm_handler_brsf_resp = {
	AT_TYPE_RESP, "+BRSF", rfcomm_handler_brsf_resp_cb };
static const struct rfcomm_handler rfcomm_handler_vgm_set = {
	AT_TYPE_CMD_SET, "+VGM", rfcomm_handler_vgm_set_cb };
static const struct rfcomm_handler rfcomm_handler_vgs_set = {
	AT_TYPE_CMD_SET, "+VGS", rfcomm_handler_vgs_set_cb };
static const struct rfcomm_handler rfcomm_handler_btrh_get = {
	AT_TYPE_CMD_GET, "+BTRH", rfcomm_handler_btrh_get_cb };
static const struct rfcomm_handler rfcomm_handler_bcs_set = {
	AT_TYPE_CMD_SET, "+BCS", rfcomm_handler_bcs_set_cb };
static const struct rfcomm_handler rfcomm_handler_bcs_resp = {
	AT_TYPE_RESP, "+BCS", rfcomm_handler_bcs_resp_cb };
static const struct rfcomm_handler rfcomm_handler_bac_set = {
	AT_TYPE_CMD_SET, "+BAC", rfcomm_handler_bac_set_cb };
static const struct rfcomm_handler rfcomm_handler_iphoneaccev_set = {
	AT_TYPE_CMD_SET, "+IPHONEACCEV", rfcomm_handler_iphoneaccev_set_cb };
static const struct rfcomm_handler rfcomm_handler_xapl_set = {
	AT_TYPE_CMD_SET, "+XAPL", rfcomm_handler_xapl_set_cb };

/**
 * Get callback (if available) for given AT message. */
static rfcomm_callback *rfcomm_get_callback(const struct bt_at *at) {

	static const struct rfcomm_handler *handlers[] = {
		&rfcomm_handler_cind_test,
		&rfcomm_handler_cind_get,
		&rfcomm_handler_cmer_set,
		&rfcomm_handler_ciev_resp,
		&rfcomm_handler_bia_set,
		&rfcomm_handler_brsf_set,
		&rfcomm_handler_vgm_set,
		&rfcomm_handler_vgs_set,
		&rfcomm_handler_btrh_get,
		&rfcomm_handler_bcs_set,
		&rfcomm_handler_bcs_resp,
		&rfcomm_handler_bac_set,
		&rfcomm_handler_iphoneaccev_set,
		&rfcomm_handler_xapl_set,
	};

	size_t i;

	for (i = 0; i < sizeof(handlers) / sizeof(*handlers); i++) {
		if (handlers[i]->type != at->type)
			continue;
		if (strcmp(handlers[i]->command, at->command) != 0)
			continue;
		return handlers[i]->callback;
	}

	return NULL;
}

void *rfcomm_thread(void *arg) {
	struct ba_transport *t = (struct ba_transport *)arg;

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(transport_pthread_cleanup, t);

	/* initialize structure used for synchronization */
	struct rfcomm_conn conn = {
		.state = HFP_DISCONNECTED,
		.state_prev = HFP_DISCONNECTED,
		.mic_gain = t->rfcomm.sco->sco.mic_gain,
		.spk_gain = t->rfcomm.sco->sco.spk_gain,
		.t = t,
	};

	struct at_reader reader = { .next = NULL };
	struct pollfd pfds[] = {
		{ t->sig_fd[0], POLLIN, 0 },
		{ t->bt_fd, POLLIN, 0 },
	};

	debug("Starting RFCOMM loop: %s",
			bluetooth_profile_to_string(t->profile, t->codec));
	for (;;) {

		/* During normal operation, RFCOMM should block indefinitely. However,
		 * in the HFP-HF mode, service level connection has to be initialized
		 * by ourself. In order to do this reliably, we have to assume, that
		 * AG might not receive our message and will not send proper response.
		 * Hence, we will incorporate timeout, after which we will send our
		 * AT command once more. */
		int timeout = -1;

		rfcomm_callback *callback;
		char tmp[16];

		if (conn.state != HFP_CONNECTED) {

			/* If some progress has been made in the SLC procedure, reset the
			 * retries counter. */
			if (conn.state != conn.state_prev) {
				conn.state_prev = conn.state;
				conn.retries = 0;
			}

			/* If the maximal number of retries has been reached, terminate the
			 * connection. Trying indefinitely will only use up our resources. */
			if (conn.retries > RFCOMM_SLC_RETRIES) {
				errno = ETIMEDOUT;
				goto ioerror;
			}

			if (t->profile == BLUETOOTH_PROFILE_HFP_HF)
				switch (conn.state) {
				case HFP_DISCONNECTED:
					sprintf(tmp, "%u", config.hfp.features_rfcomm_hf);
					if (rfcomm_write_at(pfds[1].fd, AT_TYPE_CMD_SET, "+BRSF", tmp) == -1)
						goto ioerror;
					conn.handler = &rfcomm_handler_brsf_resp;
					break;
				case HFP_SLC_BRSF_SET:
					conn.handler = &rfcomm_handler_resp_ok;
					break;
				case HFP_SLC_BRSF_SET_OK:
					if (t->rfcomm.hfp_features & HFP_AG_FEAT_CODEC) {
						if (rfcomm_write_at(pfds[1].fd, AT_TYPE_CMD_SET, "+BAC", "1") == -1)
							goto ioerror;
						conn.handler = &rfcomm_handler_resp_ok;
						break;
					}
				case HFP_SLC_BAC_SET_OK:
					if (rfcomm_write_at(pfds[1].fd, AT_TYPE_CMD_TEST, "+CIND", NULL) == -1)
						goto ioerror;
					conn.handler = &rfcomm_handler_cind_resp_test;
					break;
				case HFP_SLC_CIND_TEST:
					conn.handler = &rfcomm_handler_resp_ok;
					break;
				case HFP_SLC_CIND_TEST_OK:
					if (rfcomm_write_at(pfds[1].fd, AT_TYPE_CMD_GET, "+CIND", NULL) == -1)
						goto ioerror;
					conn.handler = &rfcomm_handler_cind_resp_get;
					break;
				case HFP_SLC_CIND_GET:
					conn.handler = &rfcomm_handler_resp_ok;
					break;
				case HFP_SLC_CIND_GET_OK:
					/* Activate indicator events reporting. The +CMER specification is
					 * as follows: AT+CMER=[<mode>[,<keyp>[,<disp>[,<ind>[,<bfr>]]]]] */
					if (rfcomm_write_at(pfds[1].fd, AT_TYPE_CMD_SET, "+CMER", "3,0,0,1,0") == -1)
						goto ioerror;
					conn.handler = &rfcomm_handler_resp_ok;
					break;
				case HFP_SLC_CMER_SET_OK:
					rfcomm_set_hfp_state(&conn, HFP_SLC_CONNECTED);
				case HFP_SLC_CONNECTED:
					if (t->rfcomm.hfp_features & HFP_AG_FEAT_CODEC)
						break;
				case HFP_CC_BCS_SET:
				case HFP_CC_BCS_SET_OK:
				case HFP_CC_CONNECTED:
					rfcomm_set_hfp_state(&conn, HFP_CONNECTED);
				case HFP_CONNECTED:
					bluealsa_ctl_event(BA_EVENT_TRANSPORT_ADDED);
				}

			if (t->profile == BLUETOOTH_PROFILE_HFP_AG)
				switch (conn.state) {
				case HFP_DISCONNECTED:
				case HFP_SLC_BRSF_SET:
				case HFP_SLC_BRSF_SET_OK:
				case HFP_SLC_BAC_SET_OK:
				case HFP_SLC_CIND_TEST:
				case HFP_SLC_CIND_TEST_OK:
				case HFP_SLC_CIND_GET:
				case HFP_SLC_CIND_GET_OK:
					break;
				case HFP_SLC_CMER_SET_OK:
					rfcomm_set_hfp_state(&conn, HFP_SLC_CONNECTED);
				case HFP_SLC_CONNECTED:
					if (t->rfcomm.hfp_features & HFP_HF_FEAT_CODEC) {
						if (rfcomm_write_at(pfds[1].fd, AT_TYPE_RESP, "+BCS", "1") == -1)
							goto ioerror;
						t->rfcomm.sco->codec = HFP_CODEC_CVSD;
						conn.handler = &rfcomm_handler_bcs_set;
						break;
					}
				case HFP_CC_BCS_SET:
				case HFP_CC_BCS_SET_OK:
				case HFP_CC_CONNECTED:
					rfcomm_set_hfp_state(&conn, HFP_CONNECTED);
				case HFP_CONNECTED:
					bluealsa_ctl_event(BA_EVENT_TRANSPORT_ADDED);
				}

			if (conn.handler != NULL) {
				timeout = RFCOMM_SLC_TIMEOUT;
				conn.retries++;
			}

		}

		/* skip poll() since we've got unprocessed data */
		if (reader.next != NULL)
			goto read;

		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

		switch (poll(pfds, sizeof(pfds) / sizeof(*pfds), timeout)) {
		case 0:
			debug("RFCOMM poll timeout");
			continue;
		case -1:
			error("RFCOMM poll error: %s", strerror(errno));
			goto fail;
		}

		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

		if (pfds[0].revents & POLLIN) {
			/* dispatch incoming event */

			enum ba_transport_signal sig = -1;
			if (read(pfds[0].fd, &sig, sizeof(sig)) != sizeof(sig))
				warn("Couldn't read signal: %s", strerror(errno));

			switch (sig) {
			case TRANSPORT_SET_VOLUME:
				if (conn.mic_gain != t->rfcomm.sco->sco.mic_gain) {
					char tmp[16];
					int gain = conn.mic_gain = t->rfcomm.sco->sco.mic_gain;
					debug("Setting microphone gain: %d", gain);
					sprintf(tmp, "+VGM=%d", gain);
					if (rfcomm_write_at(pfds[1].fd, AT_TYPE_RESP, NULL, tmp) == -1)
						goto ioerror;
				}
				if (conn.spk_gain != t->rfcomm.sco->sco.spk_gain) {
					char tmp[16];
					int gain = conn.spk_gain = t->rfcomm.sco->sco.spk_gain;
					debug("Setting speaker gain: %d", gain);
					sprintf(tmp, "+VGS=%d", gain);
					if (rfcomm_write_at(pfds[1].fd, AT_TYPE_RESP, NULL, tmp) == -1)
						goto ioerror;
				}
				break;
			case TRANSPORT_SEND_RFCOMM: {
				char cmd[32];
				if (read(pfds[0].fd, cmd, sizeof(cmd)) != sizeof(cmd))
					warn("Couldn't read RFCOMM command: %s", strerror(errno));
				if (rfcomm_write_at(pfds[1].fd, AT_TYPE_RAW, cmd, NULL) == -1)
					goto ioerror;
				break;
			}
			default:
				break;
			}

		}

		if (pfds[1].revents & POLLIN) {
			/* read data from the RFCOMM */

read:
			if (rfcomm_read_at(pfds[1].fd, &reader) == -1)
				switch (errno) {
				case EBADMSG:
					warn("Invalid AT message: %s", reader.next);
					reader.next = NULL;
					continue;
				default:
					goto ioerror;
				}

			/* use predefined callback, otherwise get generic one */
			if (conn.handler != NULL && conn.handler->type == reader.at.type &&
					strcmp(conn.handler->command, reader.at.command) == 0) {
				callback = conn.handler->callback;
				conn.handler = NULL;
			}
			else
				callback = rfcomm_get_callback(&reader.at);

			if (callback != NULL) {
				if (callback(&conn, &reader.at) == -1)
					goto ioerror;
			}
			else {
				warn("Unsupported AT message: %s: command:%s, value:%s",
						at_type2str(reader.at.type), reader.at.command, reader.at.value);
				if (reader.at.type != AT_TYPE_RESP)
					if (rfcomm_write_at(pfds[1].fd, AT_TYPE_RESP, NULL, "ERROR") == -1)
						goto ioerror;
			}

		}
		else if (pfds[1].revents & (POLLERR | POLLHUP)) {
			errno = ECONNRESET;
			goto ioerror;
		}

		continue;

ioerror:
		switch (errno) {
		case ECONNABORTED:
		case ECONNRESET:
		case ENOTCONN:
		case ENOTSUP:
		case ETIMEDOUT:
			/* exit the thread upon socket disconnection */
			debug("RFCOMM disconnected: %s", strerror(errno));
			goto fail;
		default:
			error("RFCOMM IO error: %s", strerror(errno));
		}
	}

fail:
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_pop(1);
	return NULL;
}
