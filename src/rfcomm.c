/*
 * BlueALSA - rfcomm.c
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "rfcomm.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ba-adapter.h"
#include "ba-device.h"
#include "ba-transport.h"
#include "bluealsa-dbus.h"
#include "bluealsa.h"
#include "utils.h"
#include "shared/defs.h"
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

		if (len == 0) {
			errno = ECONNRESET;
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
static void rfcomm_set_hfp_state(struct rfcomm_conn *c, enum hfp_slc_state state) {
	debug("%s state transition: %d -> %d",
			ba_transport_type_to_string(c->t->type), c->state, state);
	c->state = state;
}

/**
 * Handle AT command response code. */
static int rfcomm_handler_resp_ok_cb(struct rfcomm_conn *c, const struct bt_at *at) {

	c->handler_resp_ok_success = strcmp(at->value, "OK") == 0;

	/* advance service level connection state */
	if (c->handler_resp_ok_success && c->state != HFP_SLC_CONNECTED)
		rfcomm_set_hfp_state(c, c->handler_resp_ok_new_state);

	if (!c->handler_resp_ok_success)
		c->handler = NULL;

	return 0;
}

/**
 * TEST: Standard indicator update AT command */
static int rfcomm_handler_cind_test_cb(struct rfcomm_conn *c, const struct bt_at *at) {
	(void)at;
	const int fd = c->t->bt_fd;

	/* NOTE: The order of indicators in the CIND response message
	 *       has to be consistent with the hfp_ind enumeration. */
	if (rfcomm_write_at(fd, AT_TYPE_RESP, "+CIND",
				"(\"service\",(0-1))"
				",(\"call\",(0,1))"
				",(\"callsetup\",(0-3))"
				",(\"callheld\",(0-2))"
				",(\"signal\",(0-5))"
				",(\"roam\",(0-1))"
				",(\"battchg\",(0-5))"
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
	const int battchg = config.battery.available ? (config.battery.level + 1) / 17 : 5;
	char tmp[32];

	sprintf(tmp, "0,0,0,0,0,0,%d", battchg);
	if (rfcomm_write_at(fd, AT_TYPE_RESP, "+CIND", tmp) == -1)
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
		warn("Couldn't parse AG indicators: %s", at->value);
	if (c->state < HFP_SLC_CIND_TEST)
		rfcomm_set_hfp_state(c, HFP_SLC_CIND_TEST);
	return 0;
}

/**
 * RESP: Standard indicator update AT command */
static int rfcomm_handler_cind_resp_get_cb(struct rfcomm_conn *c, const struct bt_at *at) {

	struct ba_transport * const t = c->t;
	struct ba_device * const d = t->d;
	char *tmp = at->value;
	size_t i;

	/* parse response for the +CIND GET command */
	for (i = 0; i < ARRAYSIZE(c->hfp_ind_map); i++) {
		t->rfcomm.hfp_inds[c->hfp_ind_map[i]] = atoi(tmp);
		if (c->hfp_ind_map[i] == HFP_IND_BATTCHG) {
			d->battery_level = atoi(tmp) * 100 / 5;
			bluealsa_dbus_rfcomm_update(t, BA_DBUS_RFCOMM_UPDATE_BATTERY);
		}
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

	const char *resp = "OK";
	const int fd = c->t->bt_fd;

	if (at_parse_cmer(at->value, c->hfp_cmer) == -1) {
		warn("Couldn't parse CMER setup: %s", at->value);
		resp = "ERROR";
	}

	if (rfcomm_write_at(fd, AT_TYPE_RESP, NULL, resp) == -1)
		return -1;

	if (c->state < HFP_SLC_CMER_SET_OK)
		rfcomm_set_hfp_state(c, HFP_SLC_CMER_SET_OK);

	return 0;
}

/**
 * RESP: Standard indicator events reporting unsolicited result code */
static int rfcomm_handler_ciev_resp_cb(struct rfcomm_conn *c, const struct bt_at *at) {

	struct ba_transport * const t = c->t;
	struct ba_device * const d = t->d;
	unsigned int index;
	unsigned int value;

	if (sscanf(at->value, "%u,%u", &index, &value) == 2 &&
			--index < ARRAYSIZE(c->hfp_ind_map)) {
		t->rfcomm.hfp_inds[c->hfp_ind_map[index]] = value;
		switch (c->hfp_ind_map[index]) {
		case HFP_IND_BATTCHG:
			d->battery_level = value * 100 / 5;
			bluealsa_dbus_rfcomm_update(t, BA_DBUS_RFCOMM_UPDATE_BATTERY);
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

	struct ba_transport * const t = c->t;
	const char *resp = "OK";

	if (at_parse_bia(at->value, t->rfcomm.hfp_inds_state) == -1) {
		warn("Couldn't parse BIA indicators activation: %s", at->value);
		resp = "ERROR";
	}

	if (rfcomm_write_at(t->bt_fd, AT_TYPE_RESP, NULL, resp) == -1)
		return -1;
	return 0;
}

/**
 * SET: Bluetooth Retrieve Supported Features */
static int rfcomm_handler_brsf_set_cb(struct rfcomm_conn *c, const struct bt_at *at) {

	struct ba_transport * const t = c->t;
	struct ba_transport * const t_sco = t->rfcomm.sco;
	const int fd = t->bt_fd;
	char tmp[16];

	t->rfcomm.hfp_features = atoi(at->value);

	/* If codec negotiation is not supported in the HF, the AT+BAC
	 * command will not be sent. So, we can assume default codec. */
	if (!(t->rfcomm.hfp_features & HFP_HF_FEAT_CODEC))
		ba_transport_update_codec(t_sco, HFP_CODEC_CVSD);

	sprintf(tmp, "%u", ba_adapter_get_hfp_features_ag(t->d->a));
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
	struct ba_transport * const t_sco = t->rfcomm.sco;
	t->rfcomm.hfp_features = atoi(at->value);

	/* codec negotiation is not supported in the AG */
	if (!(t->rfcomm.hfp_features & HFP_AG_FEAT_CODEC))
		ba_transport_update_codec(t_sco, HFP_CODEC_CVSD);

	if (c->state < HFP_SLC_BRSF_SET)
		rfcomm_set_hfp_state(c, HFP_SLC_BRSF_SET);

	return 0;
}

/**
 * SET: Noise Reduction and Echo Canceling */
static int rfcomm_handler_nrec_set_cb(struct rfcomm_conn *c, const struct bt_at *at) {
	(void)at;
	/* Currently, we are not supporting Noise Reduction & Echo Canceling,
	 * so just acknowledge this SET request with "ERROR" response code. */
	if (rfcomm_write_at(c->t->bt_fd, AT_TYPE_RESP, NULL, "ERROR") == -1)
		return -1;
	return 0;
}

/**
 * SET: Gain of Microphone */
static int rfcomm_handler_vgm_set_cb(struct rfcomm_conn *c, const struct bt_at *at) {

	struct ba_transport * const t = c->t;
	struct ba_transport * const t_sco = t->rfcomm.sco;
	const int fd = t->bt_fd;

	t_sco->sco.mic_pcm.volume[0].level = c->gain_mic = atoi(at->value);
	if (rfcomm_write_at(fd, AT_TYPE_RESP, NULL, "OK") == -1)
		return -1;

	bluealsa_dbus_pcm_update(&t_sco->sco.mic_pcm, BA_DBUS_PCM_UPDATE_VOLUME);
	return 0;
}

/**
 * RESP: Gain of Microphone */
static int rfcomm_handler_vgm_resp_cb(struct rfcomm_conn *c, const struct bt_at *at) {

	struct ba_transport * const t = c->t;
	struct ba_transport * const t_sco = t->rfcomm.sco;

	t_sco->sco.mic_pcm.volume[0].level = c->gain_mic = atoi(at->value);
	bluealsa_dbus_pcm_update(&t_sco->sco.mic_pcm, BA_DBUS_PCM_UPDATE_VOLUME);
	return 0;
}

/**
 * SET: Gain of Speaker */
static int rfcomm_handler_vgs_set_cb(struct rfcomm_conn *c, const struct bt_at *at) {

	struct ba_transport * const t = c->t;
	struct ba_transport * const t_sco = t->rfcomm.sco;
	const int fd = t->bt_fd;

	t_sco->sco.spk_pcm.volume[0].level = c->gain_spk = atoi(at->value);
	if (rfcomm_write_at(fd, AT_TYPE_RESP, NULL, "OK") == -1)
		return -1;

	bluealsa_dbus_pcm_update(&t_sco->sco.spk_pcm, BA_DBUS_PCM_UPDATE_VOLUME);
	return 0;
}

/**
 * RESP: Gain of Speaker */
static int rfcomm_handler_vgs_resp_cb(struct rfcomm_conn *c, const struct bt_at *at) {

	struct ba_transport * const t = c->t;
	struct ba_transport * const t_sco = t->rfcomm.sco;

	t_sco->sco.spk_pcm.volume[0].level = c->gain_spk = atoi(at->value);
	bluealsa_dbus_pcm_update(&t_sco->sco.spk_pcm, BA_DBUS_PCM_UPDATE_VOLUME);
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
 * SET: Bluetooth Codec Connection */
static int rfcomm_handler_bcc_cmd_cb(struct rfcomm_conn *c, const struct bt_at *at) {
	(void)at;
	/* TODO: Start Codec Connection procedure because HF wants to send audio. */
	if (rfcomm_write_at(c->t->bt_fd, AT_TYPE_RESP, NULL, "ERROR") == -1)
		return -1;
	return 0;
}

/**
 * SET: Bluetooth Codec Selection */
static int rfcomm_handler_bcs_set_cb(struct rfcomm_conn *c, const struct bt_at *at) {

	struct ba_transport * const t = c->t;
	struct ba_transport * const t_sco = t->rfcomm.sco;
	const int fd = t->bt_fd;
	int codec;

	if ((codec = atoi(at->value)) != c->codec) {
		warn("Codec not acknowledged: %s != %d", at->value, c->codec);
		if (rfcomm_write_at(fd, AT_TYPE_RESP, NULL, "ERROR") == -1)
			return -1;
		goto final;
	}

	if (rfcomm_write_at(fd, AT_TYPE_RESP, NULL, "OK") == -1)
		return -1;

	/* Codec negotiation process is complete. Update transport and
	 * notify connected clients, that transport has been changed. */
	ba_transport_update_codec(t_sco, codec);
	bluealsa_dbus_pcm_update(&t_sco->sco.spk_pcm,
			BA_DBUS_PCM_UPDATE_SAMPLING | BA_DBUS_PCM_UPDATE_CODEC);
	bluealsa_dbus_pcm_update(&t_sco->sco.mic_pcm,
			BA_DBUS_PCM_UPDATE_SAMPLING | BA_DBUS_PCM_UPDATE_CODEC);

final:
	pthread_cond_signal(&t->rfcomm.codec_selection_completed);
	return 0;
}

static int rfcomm_handler_resp_bcs_ok_cb(struct rfcomm_conn *c, const struct bt_at *at) {

	struct ba_transport * const t = c->t;
	struct ba_transport * const t_sco = t->rfcomm.sco;

	if (rfcomm_handler_resp_ok_cb(c, at) == -1)
		return -1;

	if (!c->handler_resp_ok_success) {
		warn("Codec selection not finalized: %d", c->codec);
		goto final;
	}

	/* Finalize codec selection process and notify connected clients, that
	 * transport has been changed. Note, that this event might be emitted
	 * for an active transport - switching initiated by Audio Gateway. */
	ba_transport_update_codec(t_sco, c->codec);
	bluealsa_dbus_pcm_update(&t_sco->sco.spk_pcm,
			BA_DBUS_PCM_UPDATE_SAMPLING | BA_DBUS_PCM_UPDATE_CODEC);
	bluealsa_dbus_pcm_update(&t_sco->sco.mic_pcm,
			BA_DBUS_PCM_UPDATE_SAMPLING | BA_DBUS_PCM_UPDATE_CODEC);

final:
	pthread_cond_signal(&t->rfcomm.codec_selection_completed);
	return 0;
}

/**
 * RESP: Bluetooth Codec Selection */
static int rfcomm_handler_bcs_resp_cb(struct rfcomm_conn *c, const struct bt_at *at) {

	static const struct rfcomm_handler handler = {
		AT_TYPE_RESP, "", rfcomm_handler_resp_bcs_ok_cb };
	struct ba_transport * const t = c->t;
	const int fd = t->bt_fd;

	c->codec = atoi(at->value);
	if (rfcomm_write_at(fd, AT_TYPE_CMD_SET, "+BCS", at->value) == -1)
		return -1;
	c->handler = &handler;

	return 0;
}

/**
 * SET: Bluetooth Available Codecs */
static int rfcomm_handler_bac_set_cb(struct rfcomm_conn *c, const struct bt_at *at) {

	const int fd = c->t->bt_fd;
	char *tmp = at->value - 1;

	do {
		tmp += 1;
#if ENABLE_MSBC
		if (atoi(tmp) == HFP_CODEC_MSBC)
			c->msbc = true;
#endif
	} while ((tmp = strchr(tmp, ',')) != NULL);

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
	struct ba_device * const d = t->d;
	const int fd = t->bt_fd;

	char *ptr = at->value;
	size_t count = atoi(strsep(&ptr, ","));
	char tmp;

	while (count-- && ptr != NULL)
		switch (tmp = *strsep(&ptr, ",")) {
		case '1':
			if (ptr != NULL) {
				d->battery_level = atoi(strsep(&ptr, ",")) * 100 / 9;
				bluealsa_dbus_rfcomm_update(t, BA_DBUS_RFCOMM_UPDATE_BATTERY);
			}
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
	struct ba_device * const d = t->d;
	const int fd = t->bt_fd;

	unsigned int vendor, product;
	char version[sizeof(d->xapl.software_version)];
	char resp[32];
	char *tmp;

	if ((tmp = strrchr(at->value, ',')) == NULL) {
		warn("Invalid +XAPL value: %s", at->value);
		if (rfcomm_write_at(fd, AT_TYPE_RESP, NULL, "ERROR") == -1)
			return -1;
		return 0;
	}

	d->xapl.features = atoi(tmp + 1);
	*tmp = '\0';

	if (sscanf(at->value, "%x-%x-%7s", &vendor, &product, version) != 3)
		warn("Couldn't parse +XAPL vendor and product: %s", at->value);

	d->xapl.vendor_id = vendor;
	d->xapl.product_id = product;
	strcpy(d->xapl.software_version, version);

	sprintf(resp, "+XAPL=BlueALSA,%u", config.hfp.xapl_features);
	if (rfcomm_write_at(fd, AT_TYPE_RESP, NULL, resp) == -1)
		return -1;
	if (rfcomm_write_at(fd, AT_TYPE_RESP, NULL, "OK") == -1)
		return -1;
	return 0;
}

/**
 * RESP: Apple Ext: Enable custom AT commands from an accessory */
static int rfcomm_handler_xapl_resp_cb(struct rfcomm_conn *c, const struct bt_at *at) {

	static const struct rfcomm_handler handler = {
		AT_TYPE_RESP, "", rfcomm_handler_resp_ok_cb };
	struct ba_transport * const t = c->t;
	struct ba_device * const d = t->d;
	char *tmp;

	if ((tmp = strrchr(at->value, ',')) == NULL)
		return -1;

	d->xapl.features = atoi(tmp + 1);
	c->handler = &handler;

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
static const struct rfcomm_handler rfcomm_handler_nrec_set = {
	AT_TYPE_CMD_SET, "+NREC", rfcomm_handler_nrec_set_cb };
static const struct rfcomm_handler rfcomm_handler_vgm_set = {
	AT_TYPE_CMD_SET, "+VGM", rfcomm_handler_vgm_set_cb };
static const struct rfcomm_handler rfcomm_handler_vgm_resp = {
	AT_TYPE_RESP, "+VGM", rfcomm_handler_vgm_resp_cb };
static const struct rfcomm_handler rfcomm_handler_vgs_set = {
	AT_TYPE_CMD_SET, "+VGS", rfcomm_handler_vgs_set_cb };
static const struct rfcomm_handler rfcomm_handler_vgs_resp = {
	AT_TYPE_RESP, "+VGS", rfcomm_handler_vgs_resp_cb };
static const struct rfcomm_handler rfcomm_handler_btrh_get = {
	AT_TYPE_CMD_GET, "+BTRH", rfcomm_handler_btrh_get_cb };
static const struct rfcomm_handler rfcomm_handler_bcc_cmd = {
	AT_TYPE_CMD, "+BCC", rfcomm_handler_bcc_cmd_cb };
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
static const struct rfcomm_handler rfcomm_handler_xapl_resp = {
	AT_TYPE_RESP, "+XAPL", rfcomm_handler_xapl_resp_cb };

/**
 * Get callback (if available) for given AT message. */
static rfcomm_callback *rfcomm_get_callback(const struct bt_at *at) {

	static const struct rfcomm_handler *handlers[] = {
		&rfcomm_handler_resp_ok,
		&rfcomm_handler_cind_test,
		&rfcomm_handler_cind_get,
		&rfcomm_handler_cmer_set,
		&rfcomm_handler_ciev_resp,
		&rfcomm_handler_bia_set,
		&rfcomm_handler_brsf_set,
		&rfcomm_handler_nrec_set,
		&rfcomm_handler_vgm_set,
		&rfcomm_handler_vgm_resp,
		&rfcomm_handler_vgs_set,
		&rfcomm_handler_vgs_resp,
		&rfcomm_handler_btrh_get,
		&rfcomm_handler_bcc_cmd,
		&rfcomm_handler_bcs_set,
		&rfcomm_handler_bcs_resp,
		&rfcomm_handler_bac_set,
		&rfcomm_handler_iphoneaccev_set,
		&rfcomm_handler_xapl_set,
		&rfcomm_handler_xapl_resp,
	};

	size_t i;

	for (i = 0; i < ARRAYSIZE(handlers); i++) {
		if (handlers[i]->type != at->type)
			continue;
		if (strcmp(handlers[i]->command, at->command) != 0)
			continue;
		return handlers[i]->callback;
	}

	return NULL;
}

#if ENABLE_MSBC
/**
 * Try to setup HFP codec connection. */
static int rfcomm_set_hfp_codec(struct rfcomm_conn *c, uint16_t codec) {

	struct ba_transport * const t = c->t;
	const int fd = t->bt_fd;
	char tmp[16];

	debug("%s setting codec: %s",
			ba_transport_type_to_string(t->type),
			codec == HFP_CODEC_MSBC ? "mSBC" : "CVSD");

	/* Codec selection can be requested only after Service Level Connection
	 * establishment, and make sense only if mSBC encoding is supported. */
	if (c->state != HFP_SLC_CONNECTED || !c->msbc) {
		/* If codec selection was requested by some other thread by calling the
		 * ba_transport_select_codec(), we have to signal it that the selection
		 * procedure has been completed. */
		pthread_cond_signal(&t->rfcomm.codec_selection_completed);
		return 0;
	}

	/* for AG request codec selection using unsolicited response code */
	if (t->type.profile & BA_TRANSPORT_PROFILE_HFP_AG) {
		sprintf(tmp, "%d", codec);
		if (rfcomm_write_at(fd, AT_TYPE_RESP, "+BCS", tmp) == -1)
			return -1;
		c->codec = codec;
		c->handler = &rfcomm_handler_bcs_set;
		return 0;
	}

	/* TODO: Send codec connection initialization request to AG. */
	pthread_cond_signal(&t->rfcomm.codec_selection_completed);
	return 0;
}
#endif

/**
 * Notify connected BT device about host battery level change. */
static int rfcomm_notify_battery_level_change(struct rfcomm_conn *c) {

	struct ba_transport * const t = c->t;
	const int fd = t->bt_fd;
	char tmp[32];

	/* for HFP-AG return battery level indicator if reporting is enabled */
	if (t->type.profile & BA_TRANSPORT_PROFILE_HFP_AG &&
			c->hfp_cmer[3] > 0 && t->rfcomm.hfp_inds_state[HFP_IND_BATTCHG]) {
		sprintf(tmp, "%d,%d", HFP_IND_BATTCHG, (config.battery.level + 1) / 17);
		return rfcomm_write_at(fd, AT_TYPE_RESP, "+CIND", tmp);
	}

	if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_HF &&
			t->d->xapl.features & (XAPL_FEATURE_BATTERY | XAPL_FEATURE_DOCKING)) {
		sprintf(tmp, "2,1,%d,2,0", (config.battery.level + 1) / 10);
		if (rfcomm_write_at(fd, AT_TYPE_CMD_SET, "+IPHONEACCEV", tmp) == -1)
			return -1;
		c->handler = &rfcomm_handler_resp_ok;
	}

	return 0;
}

/**
 * Notify connected BT device about microphone volume change. */
static int rfcomm_notify_volume_change_mic(struct rfcomm_conn *c, bool force) {

	struct ba_transport * const t = c->t;
	const int gain = t->rfcomm.sco->sco.mic_pcm.volume[0].level;
	const int fd = t->bt_fd;
	char tmp[16];

	if (!force && c->gain_mic == gain)
		return 0;

	c->gain_mic = gain;
	debug("Updating microphone gain: %d", gain);

	/* for AG return unsolicited response code */
	if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_AG) {
		sprintf(tmp, "+VGM=%d", gain);
		return rfcomm_write_at(fd, AT_TYPE_RESP, NULL, tmp);
	}

	sprintf(tmp, "%d", gain);
	if (rfcomm_write_at(fd, AT_TYPE_CMD_SET, "+VGM", tmp) == -1)
		return -1;
	c->handler = &rfcomm_handler_resp_ok;

	return 0;
}

/**
 * Notify connected BT device about speaker volume change. */
static int rfcomm_notify_volume_change_spk(struct rfcomm_conn *c, bool force) {

	struct ba_transport * const t = c->t;
	const int gain = t->rfcomm.sco->sco.spk_pcm.volume[0].level;
	const int fd = t->bt_fd;
	char tmp[16];

	if (!force && c->gain_spk == gain)
		return 0;

	c->gain_spk = gain;
	debug("Updating speaker gain: %d", gain);

	/* for AG return unsolicited response code */
	if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_AG) {
		sprintf(tmp, "+VGS=%d", gain);
		return rfcomm_write_at(fd, AT_TYPE_RESP, NULL, tmp);
	}

	sprintf(tmp, "%d", gain);
	if (rfcomm_write_at(fd, AT_TYPE_CMD_SET, "+VGS", tmp) == -1)
		return -1;
	c->handler = &rfcomm_handler_resp_ok;

	return 0;
}

void *rfcomm_thread(struct ba_transport *t) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_pthread_cleanup), t);

	/* initialize structure used for synchronization */
	struct rfcomm_conn conn = {
		.state = HFP_DISCONNECTED,
		.state_prev = HFP_DISCONNECTED,
		.codec = HFP_CODEC_UNDEFINED,
		.gain_mic = t->rfcomm.sco->sco.mic_pcm.volume[0].level,
		.gain_spk = t->rfcomm.sco->sco.spk_pcm.volume[0].level,
		.t = t,
	};

	struct at_reader reader = { .next = NULL };
	struct pollfd pfds[] = {
		{ t->sig_fd[0], POLLIN, 0 },
		{ t->bt_fd, POLLIN, 0 },
		{ -1, POLLIN, 0 },
	};

	debug("Starting loop: %s", ba_transport_type_to_string(t->type));
	for (;;) {

		/* During normal operation, RFCOMM should block indefinitely. However,
		 * in the HFP-HF mode, service level connection has to be initialized
		 * by ourself. In order to do this reliably, we have to assume, that
		 * AG might not receive our message and will not send proper response.
		 * Hence, we will incorporate timeout, after which we will send our
		 * AT command once more. */
		int timeout = RFCOMM_TIMEOUT_IDLE;

		rfcomm_callback *callback;
		char tmp[256];

		if (conn.handler != NULL)
			goto process;

		if (conn.state != HFP_SLC_CONNECTED) {

			/* If some progress has been made in the SLC procedure, reset the
			 * retries counter. */
			if (conn.state != conn.state_prev) {
				conn.state_prev = conn.state;
				conn.retries = 0;
			}

			/* If the maximal number of retries has been reached, terminate the
			 * connection. Trying indefinitely will only use up our resources. */
			if (conn.retries > RFCOMM_SLC_RETRIES) {
				error("Couldn't establish connection: Too many retries");
				errno = ETIMEDOUT;
				goto ioerror;
			}

			if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_HSP)
				/* There is not logic behind the HSP connection,
				 * simply set status as connected. */
				rfcomm_set_hfp_state(&conn, HFP_SLC_CONNECTED);

			if (t->type.profile & BA_TRANSPORT_PROFILE_HFP_HF)
				switch (conn.state) {
				case HFP_DISCONNECTED:
					sprintf(tmp, "%u", ba_adapter_get_hfp_features_hf(t->d->a));
					if (rfcomm_write_at(pfds[1].fd, AT_TYPE_CMD_SET, "+BRSF", tmp) == -1)
						goto ioerror;
					conn.handler = &rfcomm_handler_brsf_resp;
					break;
				case HFP_SLC_BRSF_SET:
					conn.handler = &rfcomm_handler_resp_ok;
					conn.handler_resp_ok_new_state = HFP_SLC_BRSF_SET_OK;
					break;
				case HFP_SLC_BRSF_SET_OK:
					if (t->rfcomm.hfp_features & HFP_AG_FEAT_CODEC) {
#if ENABLE_MSBC
						/* advertise, that we are supporting CVSD (1) and mSBC (2) */
						const char *value = BA_TEST_ESCO_SUPPORT(t->d->a) ? "1,2" : "1";
						if (rfcomm_write_at(pfds[1].fd, AT_TYPE_CMD_SET, "+BAC", value) == -1)
#else
						if (rfcomm_write_at(pfds[1].fd, AT_TYPE_CMD_SET, "+BAC", "1") == -1)
#endif
							goto ioerror;
						conn.handler = &rfcomm_handler_resp_ok;
						conn.handler_resp_ok_new_state = HFP_SLC_BAC_SET_OK;
						break;
					}
					/* fall-through */
				case HFP_SLC_BAC_SET_OK:
					if (rfcomm_write_at(pfds[1].fd, AT_TYPE_CMD_TEST, "+CIND", NULL) == -1)
						goto ioerror;
					conn.handler = &rfcomm_handler_cind_resp_test;
					break;
				case HFP_SLC_CIND_TEST:
					conn.handler = &rfcomm_handler_resp_ok;
					conn.handler_resp_ok_new_state = HFP_SLC_CIND_TEST_OK;
					break;
				case HFP_SLC_CIND_TEST_OK:
					if (rfcomm_write_at(pfds[1].fd, AT_TYPE_CMD_GET, "+CIND", NULL) == -1)
						goto ioerror;
					conn.handler = &rfcomm_handler_cind_resp_get;
					break;
				case HFP_SLC_CIND_GET:
					conn.handler = &rfcomm_handler_resp_ok;
					conn.handler_resp_ok_new_state = HFP_SLC_CIND_GET_OK;
					break;
				case HFP_SLC_CIND_GET_OK:
					/* Activate indicator events reporting. The +CMER specification is
					 * as follows: AT+CMER=[<mode>[,<keyp>[,<disp>[,<ind>[,<bfr>]]]]] */
					if (rfcomm_write_at(pfds[1].fd, AT_TYPE_CMD_SET, "+CMER", "3,0,0,1,0") == -1)
						goto ioerror;
					conn.handler = &rfcomm_handler_resp_ok;
					conn.handler_resp_ok_new_state = HFP_SLC_CMER_SET_OK;
					break;
				case HFP_SLC_CMER_SET_OK:
					rfcomm_set_hfp_state(&conn, HFP_SLC_CONNECTED);
					/* fall-through */
				case HFP_SLC_CONNECTED:
					bluealsa_dbus_pcm_update(&t->rfcomm.sco->sco.spk_pcm,
							BA_DBUS_PCM_UPDATE_SAMPLING | BA_DBUS_PCM_UPDATE_CODEC);
					bluealsa_dbus_pcm_update(&t->rfcomm.sco->sco.mic_pcm,
							BA_DBUS_PCM_UPDATE_SAMPLING | BA_DBUS_PCM_UPDATE_CODEC);
				}

			if (t->type.profile & BA_TRANSPORT_PROFILE_HFP_AG)
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
					/* fall-through */
				case HFP_SLC_CONNECTED:
					bluealsa_dbus_pcm_update(&t->rfcomm.sco->sco.spk_pcm,
							BA_DBUS_PCM_UPDATE_SAMPLING | BA_DBUS_PCM_UPDATE_CODEC);
					bluealsa_dbus_pcm_update(&t->rfcomm.sco->sco.mic_pcm,
							BA_DBUS_PCM_UPDATE_SAMPLING | BA_DBUS_PCM_UPDATE_CODEC);
				}

		}
		else if (conn.setup != HFP_SETUP_COMPLETE) {

			if (t->type.profile & BA_TRANSPORT_PROFILE_HSP_AG)
				/* We are not making any initialization setup with
				 * HSP AG. Simply mark setup as completed. */
				conn.setup = HFP_SETUP_COMPLETE;

			/* Notify audio gateway about our initial setup. This setup
			 * is dedicated for HSP and HFP, because both profiles have
			 * volume gain control and Apple accessory extension. */
			if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_HF)
				switch (conn.setup) {
				case HFP_SETUP_GAIN_MIC:
					if (rfcomm_notify_volume_change_mic(&conn, true) == -1)
						goto ioerror;
					conn.setup++;
					break;
				case HFP_SETUP_GAIN_SPK:
					if (rfcomm_notify_volume_change_spk(&conn, true) == -1)
						goto ioerror;
					conn.setup++;
					break;
				case HFP_SETUP_ACCESSORY_XAPL:
					sprintf(tmp, "%04X-%04X-%s,%u",
							config.hfp.xapl_vendor_id, config.hfp.xapl_product_id,
							config.hfp.xapl_software_version, config.hfp.xapl_features);
					if (rfcomm_write_at(t->bt_fd, AT_TYPE_CMD_SET, "+XAPL", tmp) == -1)
						goto ioerror;
					conn.handler = &rfcomm_handler_xapl_resp;
					conn.setup++;
					break;
				case HFP_SETUP_ACCESSORY_BATT:
					if (config.battery.available &&
							rfcomm_notify_battery_level_change(&conn) == -1)
						goto ioerror;
					conn.setup++;
					break;
				case HFP_SETUP_COMPLETE:
					debug("Initial connection setup completed");
				}

			/* If HFP transport codec is already selected (e.g. device
			 * does not support mSBC) mark setup as completed. */
			if (t->type.profile & BA_TRANSPORT_PROFILE_HFP_AG &&
					t->rfcomm.sco->type.codec != HFP_CODEC_UNDEFINED)
				conn.setup = HFP_SETUP_COMPLETE;

#if ENABLE_MSBC
			/* Select HFP transport codec. Please note, that this setup
			 * stage will be performed when the connection becomes idle. */
			if (t->type.profile & BA_TRANSPORT_PROFILE_HFP_AG &&
					t->rfcomm.sco->type.codec == HFP_CODEC_UNDEFINED &&
					conn.idle) {
				if (rfcomm_set_hfp_codec(&conn, HFP_CODEC_MSBC) == -1)
					goto ioerror;
				conn.setup = HFP_SETUP_COMPLETE;
			}
#endif

		}
		else {
			/* setup is complete, block infinitely */
			timeout = -1;
		}

process:
		if (conn.handler != NULL) {
			timeout = RFCOMM_TIMEOUT_ACK;
			conn.retries++;
		}

		/* skip poll() since we've got unprocessed data */
		if (reader.next != NULL)
			goto read;

		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

		conn.idle = false;
		pfds[2].fd = t->rfcomm.handler_fd;
		switch (poll(pfds, ARRAYSIZE(pfds), timeout)) {
		case 0:
			debug("RFCOMM poll timeout");
			conn.idle = true;
			continue;
		case -1:
			if (errno == EINTR)
				continue;
			error("RFCOMM poll error: %s", strerror(errno));
			goto fail;
		}

		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

		if (pfds[0].revents & POLLIN) {
			/* dispatch incoming event */
			switch (ba_transport_recv_signal(t)) {
#if ENABLE_MSBC
			case BA_TRANSPORT_SIGNAL_HFP_SET_CODEC_CVSD:
				if (rfcomm_set_hfp_codec(&conn, HFP_CODEC_CVSD) == -1)
					goto ioerror;
				break;
			case BA_TRANSPORT_SIGNAL_HFP_SET_CODEC_MSBC:
				if (rfcomm_set_hfp_codec(&conn, HFP_CODEC_MSBC) == -1)
					goto ioerror;
				break;
#endif
			case BA_TRANSPORT_SIGNAL_UPDATE_BATTERY:
				if (rfcomm_notify_battery_level_change(&conn) == -1)
					goto ioerror;
				break;
			case BA_TRANSPORT_SIGNAL_UPDATE_VOLUME:
				if (rfcomm_notify_volume_change_mic(&conn, false) == -1)
					goto ioerror;
				if (rfcomm_notify_volume_change_spk(&conn, false) == -1)
					goto ioerror;
				break;
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
			bool predefined_callback = false;
			if (conn.handler != NULL && conn.handler->type == reader.at.type &&
					strcmp(conn.handler->command, reader.at.command) == 0) {
				callback = conn.handler->callback;
				predefined_callback = true;
				conn.handler = NULL;
			}
			else
				callback = rfcomm_get_callback(&reader.at);

			if (pfds[2].fd != -1 && !predefined_callback) {
				at_build(tmp, reader.at.type, reader.at.command, reader.at.value);
				if (write(pfds[2].fd, tmp, strlen(tmp)) == -1)
					warn("Couldn't forward AT: %s", strerror(errno));
			}

			if (callback != NULL) {
				if (callback(&conn, &reader.at) == -1)
					goto ioerror;
			}
			else if (pfds[2].fd == -1) {
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

		if (pfds[2].revents & POLLIN) {
			/* read data from the external handler */

			ssize_t ret;
			while ((ret = read(pfds[2].fd, tmp, sizeof(tmp) - 1)) == -1 &&
					errno == EINTR)
				continue;

			if (ret <= 0)
				goto ioerror_exthandler;

			tmp[ret] = '\0';
			if (rfcomm_write_at(pfds[1].fd, AT_TYPE_RAW, tmp, NULL) == -1)
				goto ioerror;

		}
		else if (pfds[2].revents & (POLLERR | POLLHUP)) {
			errno = ECONNRESET;
			goto ioerror_exthandler;
		}

		continue;

ioerror_exthandler:
		if (errno != 0)
			error("AT handler IO error: %s", strerror(errno));
		close(t->rfcomm.handler_fd);
		t->rfcomm.handler_fd = -1;
		continue;

ioerror:
		switch (errno) {
		case ECONNABORTED:
		case ECONNRESET:
		case ENOTCONN:
		case ETIMEDOUT:
		case EPIPE:
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
