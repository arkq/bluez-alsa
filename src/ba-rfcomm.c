/*
 * BlueALSA - ba-rfcomm.c
 * SPDX-FileCopyrightText: 2016-2026 BlueALSA developers
 * SPDX-License-Identifier: MIT
 */

#include "ba-rfcomm.h"

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <glib.h>

#include "ba-adapter.h"
#include "ba-config.h"
#include "ba-device.h"
#include "ba-transport.h"
#include "ba-transport-pcm.h"
#include "bluealsa-dbus.h"
#include "bluez.h"
#include "error.h"
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
 * @return ERROR_CODE_OK on success, otherwise an appropriate error code. */
static error_code_t rfcomm_read_at(int fd, struct at_reader *reader) {

	char *buffer = reader->buffer;
	char *msg = reader->next;
	char *tmp;

	/* In case of reading more than one message from the RFCOMM, we have to
	 * parse all of them before we can read from the socket once more. */
	if (msg == NULL) {

		ssize_t len;
		while ((len = read(fd, buffer, sizeof(reader->buffer))) == -1) {
			if (errno != EINTR)
				return ERROR_SYSTEM(errno);
		}

		if (len == 0)
			return ERROR_SYSTEM(ECONNRESET);

		buffer[len] = '\0';
		msg = buffer;
	}

	/* parse AT message received from the RFCOMM */
	if ((tmp = at_parse(msg, &reader->at)) == NULL) {
		reader->next = msg;
		return ERROR_SYSTEM(EBADMSG);
	}

	debug("Received AT message: %s: command=%s value=%s",
			at_type2str(reader->at.type), reader->at.command, reader->at.value);

	reader->next = tmp[0] != '\0' ? tmp : NULL;
	return ERROR_CODE_OK;
}

/**
 * Write AT message.
 *
 * @param fd RFCOMM socket file descriptor.
 * @param type Type of the AT message.
 * @param command AT command or response code.
 * @param value AT value or NULL if not applicable.
 * @return ERROR_CODE_OK on success, otherwise an appropriate error code. */
static error_code_t rfcomm_write_at(int fd, enum bt_at_type type, const char *command,
		const char *value) {

	char msg[256];
	size_t len;

	debug("Sending AT message: %s: command=%s value=%s",
			at_type2str(type),
			command != NULL ? command : "(null)",
			value != NULL ? value : "(null)");

	at_build(msg, sizeof(msg), type, command, value);
	len = strlen(msg);

	while (write(fd, msg, len) == -1) {
		if (errno != EINTR)
			return ERROR_SYSTEM(errno);
	}

	return ERROR_CODE_OK;
}

/**
 * HFP set state wrapper for debugging purposes. */
static void rfcomm_set_hfp_state(struct ba_rfcomm *r, enum hfp_slc_state state) {
	debug("RFCOMM: %s state transition: %d -> %d",
			ba_transport_debug_name(r->sco), r->state, state);
	r->state = state;
}

/**
 * Finalize HFP codec selection - signal other threads. */
static void rfcomm_finalize_codec_selection(struct ba_rfcomm *r) {

	pthread_mutex_lock(&r->sco->codec_select_client_mtx);
	r->codec_selection_done = true;
	pthread_mutex_unlock(&r->sco->codec_select_client_mtx);

	pthread_cond_signal(&r->codec_selection_cond);

}

/**
 * Handle AT command response code. */
static error_code_t rfcomm_handler_resp_ok_cb(struct ba_rfcomm *r, const struct bt_at *at) {

	r->handler_resp_ok_success = strcmp(at->value, "OK") == 0;

	/* advance service level connection state */
	if (r->handler_resp_ok_success && r->state != HFP_SLC_CONNECTED)
		rfcomm_set_hfp_state(r, r->handler_resp_ok_new_state);

	if (!r->handler_resp_ok_success)
		r->handler = NULL;

	return ERROR_CODE_OK;
}

/**
 * TEST: Standard indicator update AT command */
static error_code_t rfcomm_handler_cind_test_cb(struct ba_rfcomm *r, const struct bt_at *at) {
	(void)at;

	const int fd = r->fd;
	error_code_t ec;

	/* NOTE: The order of indicators in the CIND response message
	 *       has to be consistent with the hfp_ind enumeration. */
	if ((ec = rfcomm_write_at(fd, AT_TYPE_RESP, "+CIND",
				"(\"service\",(0,1))"
				",(\"call\",(0,1))"
				",(\"callsetup\",(0-3))"
				",(\"callheld\",(0-2))"
				",(\"signal\",(0-5))"
				",(\"roam\",(0,1))"
				",(\"battchg\",(0-5))"
			)) != ERROR_CODE_OK)
		return ec;
	if ((ec = rfcomm_write_at(fd, AT_TYPE_RESP, NULL, "OK")) != ERROR_CODE_OK)
		return ec;

	if (r->state < HFP_SLC_CIND_TEST_OK)
		rfcomm_set_hfp_state(r, HFP_SLC_CIND_TEST_OK);

	return ERROR_CODE_OK;
}

/**
 * GET: Standard indicator update AT command */
static error_code_t rfcomm_handler_cind_get_cb(struct ba_rfcomm *r, const struct bt_at *at) {
	(void)at;

	const int fd = r->fd;
	const int battchg = config.battery.available ? (config.battery.level + 1) / 17 : 5;
	error_code_t ec;
	char tmp[32];

	sprintf(tmp, "0,0,0,0,0,0,%d", battchg);
	if ((ec = rfcomm_write_at(fd, AT_TYPE_RESP, "+CIND", tmp)) != ERROR_CODE_OK)
		return ec;
	if ((ec = rfcomm_write_at(fd, AT_TYPE_RESP, NULL, "OK")) != ERROR_CODE_OK)
		return ec;

	if (r->state < HFP_SLC_CIND_GET_OK)
		rfcomm_set_hfp_state(r, HFP_SLC_CIND_GET_OK);

	return ERROR_CODE_OK;
}

/**
 * RESP: Standard indicator update AT command */
static error_code_t rfcomm_handler_cind_resp_test_cb(struct ba_rfcomm *r, const struct bt_at *at) {
	/* parse response for the +CIND TEST command */
	if (at_parse_get_cind(at->value, r->hfp_ind_map) == -1)
		warn("Couldn't parse AG indicators: %s", at->value);
	if (r->state < HFP_SLC_CIND_TEST)
		rfcomm_set_hfp_state(r, HFP_SLC_CIND_TEST);
	return ERROR_CODE_OK;
}

/**
 * RESP: Standard indicator update AT command */
static error_code_t rfcomm_handler_cind_resp_get_cb(struct ba_rfcomm *r, const struct bt_at *at) {

	struct ba_device * const d = r->sco->d;
	char *tmp = at->value;

	/* parse response for the +CIND GET command */
	for (size_t i = 0; i < ARRAYSIZE(r->hfp_ind_map); i++) {
		r->hfp_ind[r->hfp_ind_map[i]] = atoi(tmp);
		if (r->hfp_ind_map[i] == HFP_IND_BATTCHG) {
			d->battery.charge = atoi(tmp) * 100 / 5;
			bluealsa_dbus_rfcomm_update(r, BA_DBUS_RFCOMM_UPDATE_BATTERY);
			bluez_battery_provider_update(d);
		}
		if ((tmp = strchr(tmp, ',')) == NULL)
			break;
		tmp += 1;
	}

	if (r->state < HFP_SLC_CIND_GET)
		rfcomm_set_hfp_state(r, HFP_SLC_CIND_GET);

	return ERROR_CODE_OK;
}

/**
 * SET: Standard event reporting activation/deactivation AT command */
static error_code_t rfcomm_handler_cmer_set_cb(struct ba_rfcomm *r, const struct bt_at *at) {
	(void)at;

	const int fd = r->fd;
	const char *resp = "OK";
	error_code_t ec;

	if (at_parse_set_cmer(at->value, r->hfp_cmer) == -1) {
		warn("Couldn't parse CMER setup: %s", at->value);
		resp = "ERROR";
	}

	if ((ec = rfcomm_write_at(fd, AT_TYPE_RESP, NULL, resp)) != ERROR_CODE_OK)
		return ec;

	if (r->state < HFP_SLC_CMER_SET_OK)
		rfcomm_set_hfp_state(r, HFP_SLC_CMER_SET_OK);

	return ERROR_CODE_OK;
}

/**
 * RESP: Standard indicator events reporting unsolicited result code */
static error_code_t rfcomm_handler_ciev_resp_cb(struct ba_rfcomm *r, const struct bt_at *at) {

	struct ba_device * const d = r->sco->d;
	unsigned int index;
	unsigned int value;

	if (sscanf(at->value, "%u,%u", &index, &value) == 2 &&
			--index < ARRAYSIZE(r->hfp_ind_map)) {
		r->hfp_ind[r->hfp_ind_map[index]] = value;
		switch (r->hfp_ind_map[index]) {
		case HFP_IND_BATTCHG:
			d->battery.charge = value * 100 / 5;
			bluealsa_dbus_rfcomm_update(r, BA_DBUS_RFCOMM_UPDATE_BATTERY);
			bluez_battery_provider_update(d);
			break;
		default:
			break;
		}
	}

	return ERROR_CODE_OK;
}

/**
 * SET: Bluetooth Indicators Activation */
static error_code_t rfcomm_handler_bia_set_cb(struct ba_rfcomm *r, const struct bt_at *at) {

	const int fd = r->fd;
	const char *resp = "OK";

	if (at_parse_set_bia(at->value, r->hfp_ind_state) == -1) {
		warn("Couldn't parse BIA indicators activation: %s", at->value);
		resp = "ERROR";
	}

	return rfcomm_write_at(fd, AT_TYPE_RESP, NULL, resp);
}

#if !DEBUG
# define debug_ag_features(features)
#else
static void debug_ag_features(uint32_t features) {
	const char *names[32] = { NULL };
	hfp_ag_features_to_strings(features, names, ARRAYSIZE(names));
	char *tmp = g_strjoinv(", ", (char **)names);
	debug("AG features [%u]: %s", features, tmp);
	g_free(tmp);
}
#endif

#if !DEBUG
# define debug_hf_features(features)
#else
static void debug_hf_features(uint32_t features) {
	const char *names[32] = { NULL };
	hfp_hf_features_to_strings(features, names, ARRAYSIZE(names));
	char *tmp = g_strjoinv(", ", (char **)names);
	debug("HF features [%u]: %s", features, tmp);
	g_free(tmp);
}
#endif

/**
 * SET: Bluetooth Retrieve Supported Features */
static error_code_t rfcomm_handler_brsf_set_cb(struct ba_rfcomm *r, const struct bt_at *at) {

	struct ba_transport * const t_sco = r->sco;
	const int fd = r->fd;
	error_code_t ec;
	char tmp[16];

	r->hf_features = atoi(at->value);

	debug_ag_features(r->ag_features);
	debug_hf_features(r->hf_features);

	/* If codec negotiation is not supported in the HF, the AT+BAC
	 * command will not be sent. So, we can assume default codec. */
	if (!(r->hf_features & HFP_HF_FEAT_CODEC)) {
		ba_transport_set_codec(t_sco, HFP_CODEC_CVSD);
		r->hf_codecs.cvsd = true;
	}

	/* If codec negotiation is not supported on our side, the AT+BAC
	 * command will not be sent as well. In that case we will have to
	 * use some heuristic for determining which codecs are supported. */
	if (!(r->ag_features & HFP_AG_FEAT_CODEC)) {
		/* Assume that mandatory codec is supported. */
		r->hf_codecs.cvsd = true;
		/* If codec selection is supported assume that
		 * mSBC and/or LC3-SWB are supported as well. */
		if (r->hf_features & HFP_HF_FEAT_CODEC) {
#if ENABLE_MSBC
			r->hf_codecs.msbc = true;
#endif
#if ENABLE_LC3_SWB
			r->hf_codecs.lc3_swb = true;
#endif
		}
	}

	sprintf(tmp, "%u", r->ag_features);
	if ((ec = rfcomm_write_at(fd, AT_TYPE_RESP, "+BRSF", tmp)) != ERROR_CODE_OK)
		return ec;
	if ((ec = rfcomm_write_at(fd, AT_TYPE_RESP, NULL, "OK")) != ERROR_CODE_OK)
		return ec;

	if (r->state < HFP_SLC_BRSF_SET_OK)
		rfcomm_set_hfp_state(r, HFP_SLC_BRSF_SET_OK);

	return ERROR_CODE_OK;
}

/**
 * RESP: Bluetooth Retrieve Supported Features */
static error_code_t rfcomm_handler_brsf_resp_cb(struct ba_rfcomm *r, const struct bt_at *at) {

	struct ba_transport * const t_sco = r->sco;

	r->ag_features = atoi(at->value);

	debug_ag_features(r->ag_features);
	debug_hf_features(r->hf_features);

	/* codec negotiation is not supported in the AG */
	if (!(r->ag_features & HFP_AG_FEAT_CODEC))
		ba_transport_set_codec(t_sco, HFP_CODEC_CVSD);

	/* Since CVSD is a mandatory codec,
	 * we can assume that AG supports it. */
	r->ag_codecs.cvsd = true;

	/* If codec selection is supported in the AG, we can assume
	 * that mSBC and/or LC3-SWB are supported as well. */
	if (r->ag_features & HFP_AG_FEAT_CODEC) {
#if ENABLE_MSBC
		r->ag_codecs.msbc = true;
#endif
#if ENABLE_LC3_SWB
		r->ag_codecs.lc3_swb = true;
#endif
	}

	if (r->state < HFP_SLC_BRSF_SET)
		rfcomm_set_hfp_state(r, HFP_SLC_BRSF_SET);

	return ERROR_CODE_OK;
}

/**
 * SET: Noise Reduction and Echo Canceling */
static error_code_t rfcomm_handler_nrec_set_cb(struct ba_rfcomm *r, const struct bt_at *at) {
	(void)at;
	const int fd = r->fd;
	/* Currently, we are not supporting Noise Reduction & Echo Canceling,
	 * so just acknowledge this SET request with "ERROR" response code. */
	return rfcomm_write_at(fd, AT_TYPE_RESP, NULL, "ERROR");
}

/**
 * SET: Gain of Microphone */
static error_code_t rfcomm_handler_vgm_set_cb(struct ba_rfcomm *r, const struct bt_at *at) {

	struct ba_transport * const t_sco = r->sco;
	struct ba_transport_pcm *pcm = &t_sco->sco.pcm_mic;
	const int gain = r->gain_mic = atoi(at->value);
	const int fd = r->fd;

	/* skip update in case of software volume */
	if (pcm->soft_volume)
		return rfcomm_write_at(fd, AT_TYPE_RESP, NULL, "OK");

	int level = ba_transport_pcm_volume_range_to_level(gain, HFP_VOLUME_GAIN_MAX);

	pthread_mutex_lock(&pcm->mutex);
	ba_transport_pcm_volume_set(&pcm->volume[0], &level, NULL, NULL);
	pthread_mutex_unlock(&pcm->mutex);

	bluealsa_dbus_pcm_update(pcm, BA_DBUS_PCM_UPDATE_VOLUME);

	return rfcomm_write_at(fd, AT_TYPE_RESP, NULL, "OK");
}

/**
 * RESP: Gain of Microphone */
static error_code_t rfcomm_handler_vgm_resp_cb(struct ba_rfcomm *r, const struct bt_at *at) {

	struct ba_transport * const t_sco = r->sco;
	struct ba_transport_pcm *pcm = &t_sco->sco.pcm_mic;

	int gain = r->gain_mic = atoi(at->value);
	int level = ba_transport_pcm_volume_range_to_level(gain, HFP_VOLUME_GAIN_MAX);

	pthread_mutex_lock(&pcm->mutex);
	ba_transport_pcm_volume_set(&pcm->volume[0], &level, NULL, NULL);
	pthread_mutex_unlock(&pcm->mutex);

	bluealsa_dbus_pcm_update(pcm, BA_DBUS_PCM_UPDATE_VOLUME);

	return ERROR_CODE_OK;
}

/**
 * SET: Gain of Speaker */
static error_code_t rfcomm_handler_vgs_set_cb(struct ba_rfcomm *r, const struct bt_at *at) {

	struct ba_transport * const t_sco = r->sco;
	struct ba_transport_pcm *pcm = &t_sco->sco.pcm_spk;
	const int gain = r->gain_spk = atoi(at->value);
	const int fd = r->fd;

	/* skip update in case of software volume */
	if (pcm->soft_volume)
		return rfcomm_write_at(fd, AT_TYPE_RESP, NULL, "OK");

	int level = ba_transport_pcm_volume_range_to_level(gain, HFP_VOLUME_GAIN_MAX);

	pthread_mutex_lock(&pcm->mutex);
	ba_transport_pcm_volume_set(&pcm->volume[0], &level, NULL, NULL);
	pthread_mutex_unlock(&pcm->mutex);

	bluealsa_dbus_pcm_update(pcm, BA_DBUS_PCM_UPDATE_VOLUME);

	return rfcomm_write_at(fd, AT_TYPE_RESP, NULL, "OK");
}

/**
 * RESP: Gain of Speaker */
static error_code_t rfcomm_handler_vgs_resp_cb(struct ba_rfcomm *r, const struct bt_at *at) {

	struct ba_transport * const t_sco = r->sco;
	struct ba_transport_pcm *pcm = &t_sco->sco.pcm_spk;

	int gain = r->gain_spk = atoi(at->value);
	int level = ba_transport_pcm_volume_range_to_level(gain, HFP_VOLUME_GAIN_MAX);

	pthread_mutex_lock(&pcm->mutex);
	ba_transport_pcm_volume_set(&pcm->volume[0], &level, NULL, NULL);
	pthread_mutex_unlock(&pcm->mutex);

	bluealsa_dbus_pcm_update(pcm, BA_DBUS_PCM_UPDATE_VOLUME);

	return ERROR_CODE_OK;
}

/**
 * SET: Bluetooth Response and Hold Feature */
static error_code_t rfcomm_handler_btrh_get_cb(struct ba_rfcomm *r, const struct bt_at *at) {
	(void)at;
	const int fd = r->fd;
	/* Currently, we are not supporting Respond & Hold feature, so just
	 * acknowledge this GET request without reporting +BTRH status. */
	return rfcomm_write_at(fd, AT_TYPE_RESP, NULL, "OK");
}

#if ENABLE_HFP_CODEC_SELECTION
static error_code_t rfcomm_hfp_setup_codec_connection(struct ba_rfcomm *r);
#endif

/**
 * SET: Bluetooth Codec Connection */
static error_code_t rfcomm_handler_bcc_cmd_cb(struct ba_rfcomm *r, const struct bt_at *at) {
	(void)at;
	const int fd = r->fd;
	error_code_t ec;
#if ENABLE_HFP_CODEC_SELECTION
	if ((ec = rfcomm_write_at(fd, AT_TYPE_RESP, NULL, "OK")) != ERROR_CODE_OK)
		return ec;
	if ((ec = rfcomm_hfp_setup_codec_connection(r)) != ERROR_CODE_OK)
		return ec;
#else
	if ((ec = rfcomm_write_at(fd, AT_TYPE_RESP, NULL, "ERROR")) != ERROR_CODE_OK)
		return ec;
#endif
	return ERROR_CODE_OK;
}

/**
 * SET: Bluetooth Codec Selection */
static error_code_t rfcomm_handler_bcs_set_cb(struct ba_rfcomm *r, const struct bt_at *at) {

	struct ba_transport * const t_sco = r->sco;
	const int fd = r->fd;
	error_code_t ec;

	uint8_t codec_id;
	if ((codec_id = atoi(at->value)) != r->codec_id) {
		warn("Codec not acknowledged: %s != %u", at->value, r->codec_id);
		ec = rfcomm_write_at(fd, AT_TYPE_RESP, NULL, "ERROR");
		goto final;
	}

	if ((ec = rfcomm_write_at(fd, AT_TYPE_RESP, NULL, "OK")) != ERROR_CODE_OK)
		goto final;

	/* Codec negotiation process is complete. Update transport and
	 * notify connected clients, that transport has been changed. */
	ba_transport_set_codec(t_sco, codec_id);

final:
	rfcomm_finalize_codec_selection(r);
	return ec;
}

static error_code_t rfcomm_handler_resp_bcs_ok_cb(struct ba_rfcomm *r, const struct bt_at *at) {

	struct ba_transport * const t_sco = r->sco;
	error_code_t ec;

	if ((ec = rfcomm_handler_resp_ok_cb(r, at)) != ERROR_CODE_OK)
		return ec;

	if (!r->handler_resp_ok_success) {
		warn("Codec selection not finalized: %u", r->codec_id);
		ba_transport_set_codec(t_sco, HFP_CODEC_UNDEFINED);
		rfcomm_finalize_codec_selection(r);
	}

	return ERROR_CODE_OK;
}

/**
 * RESP: Bluetooth Codec Selection */
static error_code_t rfcomm_handler_bcs_resp_cb(struct ba_rfcomm *r, const struct bt_at *at) {

	static const struct ba_rfcomm_handler handler_supported = {
		AT_TYPE_RESP, "", rfcomm_handler_resp_bcs_ok_cb };
	static const struct ba_rfcomm_handler handler_unsupported = {
		AT_TYPE_RESP, "", rfcomm_handler_resp_ok_cb };

	const struct {
		uint8_t codec_id;
		bool is_supported;
	} codecs[] = {
		{ HFP_CODEC_CVSD, r->hf_codecs.cvsd },
#if ENABLE_MSBC
		{ HFP_CODEC_MSBC, r->hf_codecs.msbc },
#endif
#if ENABLE_LC3_SWB
		{ HFP_CODEC_LC3_SWB, r->hf_codecs.lc3_swb },
#endif
	};

	const int fd = r->fd;
	const uint8_t codec_id = atoi(at->value);
	error_code_t ec;
	char value[8];

	bool is_codec_supported = false;
	for (size_t i = 0; i < ARRAYSIZE(codecs); i++)
		if (codecs[i].codec_id == codec_id && codecs[i].is_supported) {
			is_codec_supported = true;
			break;
		}

	if (!is_codec_supported) {
		/* If the requested codec is not supported, we must reply with the
		 * list of codecs that we do support. */
		if ((ec = rfcomm_write_at(fd, AT_TYPE_CMD_SET, "+BAC", r->hf_bac_bcs_string)) != ERROR_CODE_OK)
			return ec;
		r->handler = &handler_unsupported;
		return ERROR_CODE_OK;
	}

	r->codec_id = codec_id;
	snprintf(value, sizeof(value), "%u", codec_id);
	if ((ec = rfcomm_write_at(fd, AT_TYPE_CMD_SET, "+BCS", value)) != ERROR_CODE_OK)
		return ec;
	r->handler = &handler_supported;

	/* The oFono AG, and possibly other AG implementations too, does not
	 * send the "OK" confirmation until it has successfully connected a
	 * SCO socket. So to support such an AG we must set the selected codec
	 * here and notify connected clients, that the transport has been
	 * changed. Note, that this event might be emitted for an active
	 * transport - codec switching initiated by Audio Gateway. */
	ba_transport_set_codec(r->sco, r->codec_id);
	rfcomm_finalize_codec_selection(r);

	return ERROR_CODE_OK;
}

/**
 * SET: Bluetooth Available Codecs */
static error_code_t rfcomm_handler_bac_set_cb(struct ba_rfcomm *r, const struct bt_at *at) {

	const int fd = r->fd;
	char *tmp = at->value - 1;
	error_code_t ec;

	/* We shall use the information on codecs available in HF
	 * from the most recently received AT+BAC command. */
	memset(&r->hf_codecs, 0, sizeof(r->hf_codecs));

	do {
		tmp += 1;
		switch (atoi(tmp)) {
		case HFP_CODEC_CVSD:
			r->hf_codecs.cvsd = true;
			break;
#if ENABLE_MSBC
		case HFP_CODEC_MSBC:
			r->hf_codecs.msbc = true;
			break;
#endif
#if ENABLE_LC3_SWB
		case HFP_CODEC_LC3_SWB:
			r->hf_codecs.lc3_swb = true;
			break;
#endif
		}
	} while ((tmp = strchr(tmp, ',')) != NULL);

	if ((ec = rfcomm_write_at(fd, AT_TYPE_RESP, NULL, "OK")) != ERROR_CODE_OK)
		goto final;

	if (r->state < HFP_SLC_BAC_SET_OK)
		rfcomm_set_hfp_state(r, HFP_SLC_BAC_SET_OK);

final:
	if (r->state == HFP_SLC_CONNECTED)
		/* We can receive the AT+BAC command as a response to AT+BSC in case of
		 * invalid codec selection. In such case, we shall finalize current codec
		 * selection procedure. */
		rfcomm_finalize_codec_selection(r);
	return ec;
}

/**
 * SET: Android Ext: XHSMICMUTE: Zebra HS3100 microphone mute */
static error_code_t rfcomm_handler_android_set_xhsmicmute(struct ba_rfcomm *r, char *value) {

	if (value == NULL)
		return ERROR_SYSTEM(EINVAL);

	struct ba_transport * const t_sco = r->sco;
	struct ba_transport_pcm *pcm = &t_sco->sco.pcm_mic;
	const bool muted = value[0] == '0' ? false : true;
	const int fd = r->fd;

	pthread_mutex_lock(&pcm->mutex);
	ba_transport_pcm_volume_set(&pcm->volume[0], NULL, NULL, &muted);
	pthread_mutex_unlock(&pcm->mutex);

	bluealsa_dbus_pcm_update(pcm, BA_DBUS_PCM_UPDATE_VOLUME);

	return rfcomm_write_at(fd, AT_TYPE_RESP, NULL, "OK");
}

/**
 * SET: Android Ext: XHSTBATSOC: Zebra HS3100 battery state of charge */
static error_code_t rfcomm_handler_android_set_xhstbatsoc(struct ba_rfcomm *r, char *value) {

	if (value == NULL)
		return ERROR_SYSTEM(EINVAL);

	struct ba_device * const d = r->sco->d;
	const int fd = r->fd;

	char *ptr = value;
	d->battery.charge = atoi(strsep(&ptr, ","));
	bluealsa_dbus_rfcomm_update(r, BA_DBUS_RFCOMM_UPDATE_BATTERY);
	bluez_battery_provider_update(d);

	return rfcomm_write_at(fd, AT_TYPE_RESP, NULL, "OK");
}

/**
 * SET: Android Ext: XHSTBATSOH: Zebra HS3100 battery state of health */
static error_code_t rfcomm_handler_android_set_xhstbatsoh(struct ba_rfcomm *r, char *value) {

	if (value == NULL)
		return ERROR_SYSTEM(EINVAL);

	struct ba_device * const d = r->sco->d;
	const int fd = r->fd;

	char *ptr = value;
	d->battery.health = atoi(strsep(&ptr, ","));
	bluealsa_dbus_rfcomm_update(r, BA_DBUS_RFCOMM_UPDATE_BATTERY);
	bluez_battery_provider_update(d);

	return rfcomm_write_at(fd, AT_TYPE_RESP, NULL, "OK");
}

/**
 * SET: Android Ext: Report various state changes */
static error_code_t rfcomm_handler_android_set_cb(struct ba_rfcomm *r, const struct bt_at *at) {

	static const struct {
		const char *name;
		error_code_t (*cb)(struct ba_rfcomm *, char *);
	} handlers[] = {
		{ "XHSMICMUTE", rfcomm_handler_android_set_xhsmicmute },
		{ "XHSTBATSOC", rfcomm_handler_android_set_xhstbatsoc },
		{ "XHSTBATSOH", rfcomm_handler_android_set_xhstbatsoh },
	};

	char *sep = ",";
	char *value = at->value;
	char *name = strsep(&value, sep);
	error_code_t ec;

	for (size_t i = 0; i < ARRAYSIZE(handlers); i++)
		if (strcmp(name, handlers[i].name) == 0) {
			if ((ec = handlers[i].cb(r, value)) == ERROR_SYSTEM(EINVAL))
				break;
			return ec;
		}

	if (value == NULL)
		sep = value = "";
	warn("Unsupported +ANDROID value: %s%s%s", name, sep, value);
	return rfcomm_write_at(r->fd, AT_TYPE_RESP, NULL, "ERROR");
}

/**
 * SET: Apple Ext: Report a headset state change */
static error_code_t rfcomm_handler_iphoneaccev_set_cb(struct ba_rfcomm *r, const struct bt_at *at) {

	struct ba_device * const d = r->sco->d;
	const int fd = r->fd;

	char *ptr = at->value;
	size_t count = atoi(strsep(&ptr, ","));
	char tmp;

	while (count-- && ptr != NULL)
		switch (tmp = *strsep(&ptr, ",")) {
		case '1':
			if (ptr != NULL) {
				d->battery.charge = atoi(strsep(&ptr, ",")) * 100 / 9;
				bluealsa_dbus_rfcomm_update(r, BA_DBUS_RFCOMM_UPDATE_BATTERY);
				bluez_battery_provider_update(d);
			}
			break;
		case '2':
			if (ptr != NULL)
				d->xapl.accev_docked = atoi(strsep(&ptr, ","));
			break;
		default:
			warn("Unsupported +IPHONEACCEV key: %c", tmp);
			strsep(&ptr, ",");
		}

	return rfcomm_write_at(fd, AT_TYPE_RESP, NULL, "OK");
}

/**
 * SET: Apple Ext: Enable custom AT commands from an accessory */
static error_code_t rfcomm_handler_xapl_set_cb(struct ba_rfcomm *r, const struct bt_at *at) {

	struct ba_device * const d = r->sco->d;
	const int fd = r->fd;
	error_code_t ec;

	if (at_parse_set_xapl(at->value, &d->xapl.vendor_id, &d->xapl.product_id,
				&d->xapl.sw_version, &d->xapl.features) == -1) {
		warn("Invalid +XAPL value: %s", at->value);
		return rfcomm_write_at(fd, AT_TYPE_RESP, NULL, "ERROR");
	}

	char resp[32];
	snprintf(resp, sizeof(resp), "+XAPL=%s,%u",
			config.hfp.xapl_product_name, config.hfp.xapl_features);

	if ((ec = rfcomm_write_at(fd, AT_TYPE_RESP, NULL, resp)) != ERROR_CODE_OK)
		return ec;
	if ((ec = rfcomm_write_at(fd, AT_TYPE_RESP, NULL, "OK")) != ERROR_CODE_OK)
		return ec;
	return ERROR_CODE_OK;
}

/**
 * RESP: Apple Ext: Enable custom AT commands from an accessory */
static error_code_t rfcomm_handler_xapl_resp_cb(struct ba_rfcomm *r, const struct bt_at *at) {

	static const struct ba_rfcomm_handler handler = {
		AT_TYPE_RESP, "", rfcomm_handler_resp_ok_cb };
	struct ba_device * const d = r->sco->d;
	char *tmp;

	if ((tmp = strrchr(at->value, ',')) == NULL)
		return ERROR_SYSTEM(EINVAL);

	d->xapl.features = atoi(tmp + 1);
	r->handler = &handler;

	return ERROR_CODE_OK;
}

static const struct ba_rfcomm_handler rfcomm_handler_resp_ok = {
	AT_TYPE_RESP, "", rfcomm_handler_resp_ok_cb };
static const struct ba_rfcomm_handler rfcomm_handler_cind_test = {
	AT_TYPE_CMD_TEST, "+CIND", rfcomm_handler_cind_test_cb };
static const struct ba_rfcomm_handler rfcomm_handler_cind_get = {
	AT_TYPE_CMD_GET, "+CIND", rfcomm_handler_cind_get_cb };
static const struct ba_rfcomm_handler rfcomm_handler_cind_resp_test = {
	AT_TYPE_RESP, "+CIND", rfcomm_handler_cind_resp_test_cb };
static const struct ba_rfcomm_handler rfcomm_handler_cind_resp_get = {
	AT_TYPE_RESP, "+CIND", rfcomm_handler_cind_resp_get_cb };
static const struct ba_rfcomm_handler rfcomm_handler_cmer_set = {
	AT_TYPE_CMD_SET, "+CMER", rfcomm_handler_cmer_set_cb };
static const struct ba_rfcomm_handler rfcomm_handler_ciev_resp = {
	AT_TYPE_RESP, "+CIEV", rfcomm_handler_ciev_resp_cb };
static const struct ba_rfcomm_handler rfcomm_handler_bia_set = {
	AT_TYPE_CMD_SET, "+BIA", rfcomm_handler_bia_set_cb };
static const struct ba_rfcomm_handler rfcomm_handler_brsf_set = {
	AT_TYPE_CMD_SET, "+BRSF", rfcomm_handler_brsf_set_cb };
static const struct ba_rfcomm_handler rfcomm_handler_brsf_resp = {
	AT_TYPE_RESP, "+BRSF", rfcomm_handler_brsf_resp_cb };
static const struct ba_rfcomm_handler rfcomm_handler_nrec_set = {
	AT_TYPE_CMD_SET, "+NREC", rfcomm_handler_nrec_set_cb };
static const struct ba_rfcomm_handler rfcomm_handler_vgm_set = {
	AT_TYPE_CMD_SET, "+VGM", rfcomm_handler_vgm_set_cb };
static const struct ba_rfcomm_handler rfcomm_handler_vgm_resp = {
	AT_TYPE_RESP, "+VGM", rfcomm_handler_vgm_resp_cb };
static const struct ba_rfcomm_handler rfcomm_handler_vgs_set = {
	AT_TYPE_CMD_SET, "+VGS", rfcomm_handler_vgs_set_cb };
static const struct ba_rfcomm_handler rfcomm_handler_vgs_resp = {
	AT_TYPE_RESP, "+VGS", rfcomm_handler_vgs_resp_cb };
static const struct ba_rfcomm_handler rfcomm_handler_btrh_get = {
	AT_TYPE_CMD_GET, "+BTRH", rfcomm_handler_btrh_get_cb };
static const struct ba_rfcomm_handler rfcomm_handler_bcc_cmd = {
	AT_TYPE_CMD, "+BCC", rfcomm_handler_bcc_cmd_cb };
static const struct ba_rfcomm_handler rfcomm_handler_bcs_set = {
	AT_TYPE_CMD_SET, "+BCS", rfcomm_handler_bcs_set_cb };
static const struct ba_rfcomm_handler rfcomm_handler_bcs_resp = {
	AT_TYPE_RESP, "+BCS", rfcomm_handler_bcs_resp_cb };
static const struct ba_rfcomm_handler rfcomm_handler_bac_set = {
	AT_TYPE_CMD_SET, "+BAC", rfcomm_handler_bac_set_cb };
static const struct ba_rfcomm_handler rfcomm_handler_android_set = {
	AT_TYPE_CMD_SET, "+ANDROID", rfcomm_handler_android_set_cb };
static const struct ba_rfcomm_handler rfcomm_handler_iphoneaccev_set = {
	AT_TYPE_CMD_SET, "+IPHONEACCEV", rfcomm_handler_iphoneaccev_set_cb };
static const struct ba_rfcomm_handler rfcomm_handler_xapl_set = {
	AT_TYPE_CMD_SET, "+XAPL", rfcomm_handler_xapl_set_cb };
static const struct ba_rfcomm_handler rfcomm_handler_xapl_resp = {
	AT_TYPE_RESP, "+XAPL", rfcomm_handler_xapl_resp_cb };

/**
 * Get callback (if available) for given AT message. */
static ba_rfcomm_callback *rfcomm_get_callback(const struct bt_at *at) {

	static const struct ba_rfcomm_handler *handlers[] = {
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
		&rfcomm_handler_android_set,
		&rfcomm_handler_iphoneaccev_set,
		&rfcomm_handler_xapl_set,
		&rfcomm_handler_xapl_resp,
	};

	for (size_t i = 0; i < ARRAYSIZE(handlers); i++) {
		if (handlers[i]->type != at->type)
			continue;
		if (strcmp(handlers[i]->command, at->command) != 0)
			continue;
		return handlers[i]->callback;
	}

	return NULL;
}

static enum ba_rfcomm_signal rfcomm_recv_signal(struct ba_rfcomm *r) {

	enum ba_rfcomm_signal sig;
	ssize_t ret;

	while ((ret = read(r->sig_fd[0], &sig, sizeof(sig))) == -1 &&
			errno == EINTR)
		continue;

	if (ret == sizeof(sig))
		return sig;

	warn("Couldn't read RFCOMM signal: %s", strerror(errno));
	return BA_RFCOMM_SIGNAL_PING;
}

#if ENABLE_HFP_CODEC_SELECTION

/**
 * Set HFP codec for the given Service Level Connection. */
static error_code_t rfcomm_hfp_set_codec(struct ba_rfcomm *r, uint8_t codec_id) {

	struct ba_transport * const t_sco = r->sco;
	const int fd = r->fd;
	error_code_t ec = ERROR_CODE_OK;

	debug("RFCOMM: %s setting codec: %s",
			ba_transport_debug_name(t_sco),
			hfp_codec_to_string(codec_id));

	/* SLC is required for codec connection */
	if (r->state != HFP_SLC_CONNECTED)
		goto fail;

	/* only AG can set codec */
	if (!(t_sco->profile & BA_TRANSPORT_PROFILE_HFP_AG))
		goto fail;

	char tmp[16];
	sprintf(tmp, "%u", codec_id);
	if ((ec = rfcomm_write_at(fd, AT_TYPE_RESP, "+BCS", tmp)) != ERROR_CODE_OK)
		goto fail;

	r->codec_id = codec_id;
	r->handler = &rfcomm_handler_bcs_set;
	return ERROR_CODE_OK;

fail:
	rfcomm_finalize_codec_selection(r);
	return ec;
}

/**
 * Try to setup HFP codec connection. */
static error_code_t rfcomm_hfp_setup_codec_connection(struct ba_rfcomm *r) {

	const struct {
		uint8_t codec_id;
		bool is_supported;
	} codecs[] = {
#if ENABLE_LC3_SWB
		{ HFP_CODEC_LC3_SWB, r->ag_codecs.lc3_swb && r->hf_codecs.lc3_swb },
#endif
#if ENABLE_MSBC
		{ HFP_CODEC_MSBC, r->ag_codecs.msbc && r->hf_codecs.msbc },
#endif
		{ HFP_CODEC_CVSD, r->ag_codecs.cvsd && r->hf_codecs.cvsd },
	};

	struct ba_transport * const t_sco = r->sco;
	const int fd = r->fd;
	error_code_t ec;

	/* SLC is required for codec connection */
	if (r->state != HFP_SLC_CONNECTED)
		return ERROR_CODE_OK;

	/* nothing to do if codec is already selected */
	if (ba_transport_get_codec(t_sco) != HFP_CODEC_UNDEFINED)
		return ERROR_CODE_OK;

	/* Only AG can initialize codec connection. So, for HF we need to request
	 * codec selection from AG by sending AT+BCC command. */
	if (t_sco->profile & BA_TRANSPORT_PROFILE_HFP_HF) {
		if ((ec = rfcomm_write_at(fd, AT_TYPE_CMD, "+BCC", NULL)) != ERROR_CODE_OK)
			return ec;
		r->handler = &rfcomm_handler_resp_ok;
		return ERROR_CODE_OK;
	}

	for (size_t i = 0; i < ARRAYSIZE(codecs); i++) {
		if (!codecs[i].is_supported)
			continue;
		if ((ec = rfcomm_hfp_set_codec(r, codecs[i].codec_id)) != ERROR_CODE_OK)
			return ec;
		break;
	}

	return ERROR_CODE_OK;
}

#endif

/**
 * Notify connected BT device about host battery level change. */
static error_code_t rfcomm_notify_battery_level_change(struct ba_rfcomm *r) {

	struct ba_transport * const t_sco = r->sco;
	const int fd = r->fd;
	error_code_t ec;
	char tmp[32];

	if (!config.battery.available)
		return ERROR_CODE_OK;

	/* for HFP-AG return battery level indicator if reporting is enabled */
	if (t_sco->profile & BA_TRANSPORT_PROFILE_HFP_AG &&
			r->hfp_cmer[3] > 0 && r->hfp_ind_state[HFP_IND_BATTCHG]) {
		const unsigned int level = config.battery.level * 6 / 100;
		sprintf(tmp, "%d,%d", HFP_IND_BATTCHG, MIN(level, 5));
		return rfcomm_write_at(fd, AT_TYPE_RESP, "+CIEV", tmp);
	}

	if (t_sco->profile & BA_TRANSPORT_PROFILE_MASK_HF &&
			t_sco->d->xapl.features & (XAPL_FEATURE_BATTERY | XAPL_FEATURE_DOCKING)) {
		const unsigned int level = config.battery.level * 10 / 100;
		sprintf(tmp, "2,1,%d,2,0", MIN(level, 9));
		if ((ec = rfcomm_write_at(fd, AT_TYPE_CMD_SET, "+IPHONEACCEV", tmp)) != ERROR_CODE_OK)
			return ec;
		r->handler = &rfcomm_handler_resp_ok;
	}

	return ERROR_CODE_OK;
}

/**
 * Notify connected BT device about microphone volume change. */
static error_code_t rfcomm_notify_volume_change_mic(struct ba_rfcomm *r, bool force) {

	struct ba_transport * const t_sco = r->sco;
	struct ba_transport_pcm *pcm = &t_sco->sco.pcm_mic;
	const int fd = r->fd;
	error_code_t ec;
	char tmp[24];

	int gain = ba_transport_pcm_volume_level_to_range(
			pcm->volume[0].level, HFP_VOLUME_GAIN_MAX);
	if (!force && r->gain_mic == gain)
		return ERROR_CODE_OK;

	r->gain_mic = gain;
	debug("Updating microphone gain: %d", gain);

	/* for AG return unsolicited response code */
	if (t_sco->profile & BA_TRANSPORT_PROFILE_MASK_AG) {
		bool is_hsp = t_sco->profile & BA_TRANSPORT_PROFILE_MASK_HSP;
		sprintf(tmp, "+VGM%c%d", is_hsp ? '=' : ':', gain);
		return rfcomm_write_at(fd, AT_TYPE_RESP, NULL, tmp);
	}

	sprintf(tmp, "%d", gain);
	if ((ec = rfcomm_write_at(fd, AT_TYPE_CMD_SET, "+VGM", tmp)) != ERROR_CODE_OK)
		return ec;
	r->handler = &rfcomm_handler_resp_ok;

	return ERROR_CODE_OK;
}

/**
 * Notify connected BT device about speaker volume change. */
static error_code_t rfcomm_notify_volume_change_spk(struct ba_rfcomm *r, bool force) {

	struct ba_transport * const t_sco = r->sco;
	struct ba_transport_pcm *pcm = &t_sco->sco.pcm_spk;
	const int fd = r->fd;
	error_code_t ec;
	char tmp[24];

	int gain = ba_transport_pcm_volume_level_to_range(
			pcm->volume[0].level, HFP_VOLUME_GAIN_MAX);
	if (!force && r->gain_spk == gain)
		return ERROR_CODE_OK;

	r->gain_spk = gain;
	debug("Updating speaker gain: %d", gain);

	/* for AG return unsolicited response code */
	if (t_sco->profile & BA_TRANSPORT_PROFILE_MASK_AG) {
		bool is_hsp = t_sco->profile & BA_TRANSPORT_PROFILE_MASK_HSP;
		sprintf(tmp, "+VGS%c%d", is_hsp ? '=' : ':', gain);
		return rfcomm_write_at(fd, AT_TYPE_RESP, NULL, tmp);
	}

	sprintf(tmp, "%d", gain);
	if ((ec = rfcomm_write_at(fd, AT_TYPE_CMD_SET, "+VGS", tmp)) != ERROR_CODE_OK)
		return ec;
	r->handler = &rfcomm_handler_resp_ok;

	return ERROR_CODE_OK;
}

/**
 * Drive the Service Level Connection state machine. */
static error_code_t rfcomm_drive_slc(struct ba_rfcomm * r) {

	struct ba_transport * const t_sco = r->sco;
	const int fd = r->fd;
	char tmp[256] = "";
	error_code_t ec;

	if (r->state == HFP_SLC_CONNECTED)
		return ERROR_CODE_OK;

	/* If some progress has been made in the SLC procedure, reset the
	 * retries counter. */
	if (r->state != r->state_prev) {
		r->state_prev = r->state;
		r->retries = 0;
	}

	/* If the maximal number of retries has been reached, terminate the
	 * connection. Trying indefinitely will only use up our resources. */
	if (r->retries > BA_RFCOMM_SLC_RETRIES) {
		error("Couldn't establish connection: Too many retries");
		return ERROR_SYSTEM(ETIMEDOUT);
	}

	if (t_sco->profile & BA_TRANSPORT_PROFILE_MASK_HSP) {
		/* There is not logic behind the HSP connection,
		 * simply set status as connected. */
		rfcomm_set_hfp_state(r, HFP_SLC_CONNECTED);
		return ERROR_CODE_OK;
	}

	if (t_sco->profile & BA_TRANSPORT_PROFILE_HFP_HF)
		switch (r->state) {
		case HFP_DISCONNECTED:
			sprintf(tmp, "%u", r->hf_features);
			if ((ec = rfcomm_write_at(fd, AT_TYPE_CMD_SET, "+BRSF", tmp)) != ERROR_CODE_OK)
				return ec;
			r->handler = &rfcomm_handler_brsf_resp;
			break;
		case HFP_SLC_BRSF_SET:
			r->handler = &rfcomm_handler_resp_ok;
			r->handler_resp_ok_new_state = HFP_SLC_BRSF_SET_OK;
			break;
		case HFP_SLC_BRSF_SET_OK:
			/* Process with codecs advertisement only if both
			 * sides support the codec negotiation feature. */
			if (r->ag_features & HFP_AG_FEAT_CODEC &&
					r->hf_features & HFP_HF_FEAT_CODEC) {
				if ((ec = rfcomm_write_at(fd, AT_TYPE_CMD_SET, "+BAC", r->hf_bac_bcs_string)) != ERROR_CODE_OK)
					return ec;
				r->handler = &rfcomm_handler_resp_ok;
				r->handler_resp_ok_new_state = HFP_SLC_BAC_SET_OK;
				break;
			}
			/* fall-through */
		case HFP_SLC_BAC_SET_OK:
			if ((ec = rfcomm_write_at(fd, AT_TYPE_CMD_TEST, "+CIND", NULL)) != ERROR_CODE_OK)
				return ec;
			r->handler = &rfcomm_handler_cind_resp_test;
			break;
		case HFP_SLC_CIND_TEST:
			r->handler = &rfcomm_handler_resp_ok;
			r->handler_resp_ok_new_state = HFP_SLC_CIND_TEST_OK;
			break;
		case HFP_SLC_CIND_TEST_OK:
			if ((ec = rfcomm_write_at(fd, AT_TYPE_CMD_GET, "+CIND", NULL)) != ERROR_CODE_OK)
				return ec;
			r->handler = &rfcomm_handler_cind_resp_get;
			break;
		case HFP_SLC_CIND_GET:
			r->handler = &rfcomm_handler_resp_ok;
			r->handler_resp_ok_new_state = HFP_SLC_CIND_GET_OK;
			break;
		case HFP_SLC_CIND_GET_OK:
			/* Activate indicator events reporting. The +CMER specification is
			 * as follows: AT+CMER=[<mode>[,<keyp>[,<disp>[,<ind>[,<bfr>]]]]] */
			if ((ec = rfcomm_write_at(fd, AT_TYPE_CMD_SET, "+CMER", "3,0,0,1,0")) != ERROR_CODE_OK)
				return ec;
			r->handler = &rfcomm_handler_resp_ok;
			r->handler_resp_ok_new_state = HFP_SLC_CMER_SET_OK;
			break;
		case HFP_SLC_CMER_SET_OK:
			rfcomm_set_hfp_state(r, HFP_SLC_CONNECTED);
			/* fall-through */
		case HFP_SLC_CONNECTED:
			/* If codec was selected during the SLC establishment,
			 * notify BlueALSA D-Bus clients about the change. */
			if (ba_transport_get_codec(t_sco) != HFP_CODEC_UNDEFINED) {
				bluealsa_dbus_pcm_update(&t_sco->sco.pcm_spk,
						BA_DBUS_PCM_UPDATE_RATE | BA_DBUS_PCM_UPDATE_CODEC);
				bluealsa_dbus_pcm_update(&t_sco->sco.pcm_mic,
						BA_DBUS_PCM_UPDATE_RATE | BA_DBUS_PCM_UPDATE_CODEC);
			}
		}

	if (t_sco->profile & BA_TRANSPORT_PROFILE_HFP_AG)
		switch (r->state) {
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
			rfcomm_set_hfp_state(r, HFP_SLC_CONNECTED);
			/* fall-through */
		case HFP_SLC_CONNECTED:
			/* If codec was selected during the SLC establishment,
			 * notify BlueALSA D-Bus clients about the change. */
			if (ba_transport_get_codec(t_sco) != HFP_CODEC_UNDEFINED) {
				bluealsa_dbus_pcm_update(&t_sco->sco.pcm_spk,
						BA_DBUS_PCM_UPDATE_RATE | BA_DBUS_PCM_UPDATE_CODEC);
				bluealsa_dbus_pcm_update(&t_sco->sco.pcm_mic,
						BA_DBUS_PCM_UPDATE_RATE | BA_DBUS_PCM_UPDATE_CODEC);
			}
		}

	return ERROR_CODE_OK;
}

/**
 * Drive the setup procedure after SLC establishment. */
static error_code_t rfcomm_drive_setup(struct ba_rfcomm * r) {

	struct ba_transport * const t_sco = r->sco;
	const int fd = r->fd;
	char tmp[256] = "";
	error_code_t ec;

	if (r->state != HFP_SLC_CONNECTED)
		return rfcomm_drive_slc(r);

	if (t_sco->profile & BA_TRANSPORT_PROFILE_HSP_AG)
		/* We are not making any initialization setup with
		 * HSP AG. Simply mark setup as completed. */
		r->setup = HFP_SETUP_COMPLETED;

	/* Notify audio gateway about our initial setup. This setup
	 * is dedicated for HSP and HFP, because both profiles have
	 * volume gain control and Apple accessory extension. */
	if (t_sco->profile & BA_TRANSPORT_PROFILE_MASK_HF)
		switch (r->setup) {
		case HFP_SETUP_GAIN_MIC:
			if ((ec = rfcomm_notify_volume_change_mic(r, true)) != ERROR_CODE_OK)
				return ec;
			r->setup++;
			break;
		case HFP_SETUP_GAIN_SPK:
			if ((ec = rfcomm_notify_volume_change_spk(r, true)) != ERROR_CODE_OK)
				return ec;
			r->setup++;
			break;
		case HFP_SETUP_ACCESSORY_XAPL:
			sprintf(tmp, "%04X-%04X-%04X,%u",
					config.hfp.xapl_vendor_id, config.hfp.xapl_product_id,
					config.hfp.xapl_sw_version, config.hfp.xapl_features);
			if ((ec = rfcomm_write_at(fd, AT_TYPE_CMD_SET, "+XAPL", tmp)) != ERROR_CODE_OK)
				return ec;
			r->handler = &rfcomm_handler_xapl_resp;
			r->setup++;
			break;
		case HFP_SETUP_ACCESSORY_BATT:
			if ((ec = rfcomm_notify_battery_level_change(r)) != ERROR_CODE_OK)
				return ec;
			r->setup++;
			break;
		case HFP_SETUP_SELECT_CODEC:
#if ENABLE_HFP_CODEC_SELECTION
			if (r->idle) {
				if ((ec = rfcomm_hfp_setup_codec_connection(r)) != ERROR_CODE_OK)
					return ec;
				r->setup++;
			}
#else
			r->setup++;
#endif
			/* fall-through */
		case HFP_SETUP_COMPLETED:
			debug("Initial connection setup completed");
		}

	/* If HFP transport codec is already selected (e.g. device
	 * does not support mSBC) mark setup as completed. */
	if (t_sco->profile & BA_TRANSPORT_PROFILE_HFP_AG &&
			ba_transport_get_codec(t_sco) != HFP_CODEC_UNDEFINED)
		r->setup = HFP_SETUP_COMPLETED;

#if ENABLE_HFP_CODEC_SELECTION
	/* Select HFP transport codec. Please note, that this setup
	 * stage will be performed when the connection becomes idle. */
	if (t_sco->profile & BA_TRANSPORT_PROFILE_HFP_AG &&
			r->idle) {
		if ((ec = rfcomm_hfp_setup_codec_connection(r)) != ERROR_CODE_OK)
			return ec;
		r->setup = HFP_SETUP_COMPLETED;
	}
#endif

	return ERROR_CODE_OK;
}

/**
 * Dispatch incoming control events. */
static error_code_t rfcomm_dispatch_control_events(struct ba_rfcomm * r) {
	error_code_t ec;
	switch (rfcomm_recv_signal(r)) {
#if ENABLE_HFP_CODEC_SELECTION
	case BA_RFCOMM_SIGNAL_HFP_SET_CODEC_CVSD:
		if (config.hfp.codecs.cvsd && (
					r->ag_features & HFP_AG_FEAT_CODEC &&
					r->hf_features & HFP_HF_FEAT_CODEC))
			return rfcomm_hfp_set_codec(r, HFP_CODEC_CVSD);
		rfcomm_finalize_codec_selection(r);
		break;
# if ENABLE_MSBC
	case BA_RFCOMM_SIGNAL_HFP_SET_CODEC_MSBC:
		if (config.hfp.codecs.msbc && (
					r->ag_features & HFP_AG_FEAT_CODEC &&
					r->ag_features & HFP_AG_FEAT_ESCO &&
					r->hf_features & HFP_HF_FEAT_CODEC &&
					r->hf_features & HFP_HF_FEAT_ESCO))
			return rfcomm_hfp_set_codec(r, HFP_CODEC_MSBC);
		rfcomm_finalize_codec_selection(r);
		break;
# endif
# if ENABLE_LC3_SWB
	case BA_RFCOMM_SIGNAL_HFP_SET_CODEC_LC3_SWB:
		if (config.hfp.codecs.lc3_swb && (
					r->ag_features & HFP_AG_FEAT_CODEC &&
					r->ag_features & HFP_AG_FEAT_ESCO &&
					r->hf_features & HFP_HF_FEAT_CODEC &&
					r->hf_features & HFP_HF_FEAT_ESCO))
			return rfcomm_hfp_set_codec(r, HFP_CODEC_LC3_SWB);
		rfcomm_finalize_codec_selection(r);
		break;
# endif
#endif
	case BA_RFCOMM_SIGNAL_UPDATE_BATTERY:
		return rfcomm_notify_battery_level_change(r);
	case BA_RFCOMM_SIGNAL_UPDATE_VOLUME:
		if ((ec = rfcomm_notify_volume_change_mic(r, false)) != ERROR_CODE_OK)
			return ec;
		return rfcomm_notify_volume_change_spk(r, false);
	default:
		break;
	}
	return ERROR_CODE_OK;
}

static void rfcomm_thread_cleanup(struct ba_rfcomm *r) {

	if (r->fd == -1)
		return;

	debug("Closing RFCOMM: %d", r->fd);

	shutdown(r->fd, SHUT_RDWR);
	close(r->fd);
	r->fd = -1;

	if (r->sco != NULL) {

		/* battery status will no longer be available */
		struct ba_device *d = r->sco->d;
		d->battery.charge = -1;
		d->battery.health = -1;
		bluealsa_dbus_rfcomm_update(r, BA_DBUS_RFCOMM_UPDATE_BATTERY);
		bluez_battery_provider_update(d);

		if (r->link_lost_quirk) {
			debug("RFCOMM link lost quirk: Destroying SCO transport");
			r->sco->sco.rfcomm = NULL;
			ba_transport_ref(r->sco);
			ba_transport_destroy(r->sco);
			ba_rfcomm_destroy(r);
			return;
		}

		ba_transport_unref(r->sco);
		r->sco = NULL;

	}

}

static void *rfcomm_thread(struct ba_rfcomm *r) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(rfcomm_thread_cleanup), r);

	sigset_t sigset;
	/* See the ba_transport_pcm_start() function for information
	 * why we have to mask all signals. */
	sigfillset(&sigset);
	pthread_sigmask(SIG_SETMASK, &sigset, NULL);

	struct at_reader reader = { .next = NULL };
	struct pollfd pfds[] = {
		{ r->sig_fd[0], POLLIN, 0 },
		{ r->fd, POLLIN, 0 },
		{ -1, POLLIN, 0 },
	};

	debug("Starting RFCOMM loop: %s", ba_transport_debug_name(r->sco));
	for (;;) {

		/* During normal operation, RFCOMM should block indefinitely. However,
		 * in the HFP-HF mode, service level connection has to be initialized
		 * by ourself. In order to do this reliably, we have to assume, that
		 * AG might not receive our message and will not send proper response.
		 * Hence, we will incorporate timeout, after which we will send our
		 * AT command once more. */
		int timeout = BA_RFCOMM_TIMEOUT_IDLE;

		ba_rfcomm_callback *callback;
		char tmp[256] = "";
		error_code_t ec;

		if (r->handler == NULL && r->setup != HFP_SETUP_COMPLETED)
			if ((ec = rfcomm_drive_setup(r)) != ERROR_CODE_OK)
				goto ioerror;

		if (r->setup == HFP_SETUP_COMPLETED)
			/* Setup is completed, block infinitely. */
			timeout = -1;

		if (r->handler != NULL) {
			timeout = BA_RFCOMM_TIMEOUT_ACK;
			r->retries++;
		}

		/* skip poll() since we've got unprocessed data */
		if (reader.next != NULL)
			goto read;

		r->idle = false;
		pfds[2].fd = r->handler_fd;

		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
		int poll_rv = poll(pfds, ARRAYSIZE(pfds), timeout);
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

		switch (poll_rv) {
		case 0:
			debug("RFCOMM poll timeout");
			r->idle = true;
			continue;
		case -1:
			if (errno == EINTR)
				continue;
			error("RFCOMM poll error: %s", strerror(errno));
			goto fail;
		}

		if (pfds[0].revents & POLLIN) {
			if ((ec = rfcomm_dispatch_control_events(r)) != ERROR_CODE_OK)
				goto ioerror;
		}

		if (pfds[1].revents & POLLIN) {
			/* read data from the RFCOMM */

read:
			if ((ec = rfcomm_read_at(pfds[1].fd, &reader)) != ERROR_CODE_OK)
				switch (ec) {
				case ERROR_SYSTEM(EBADMSG):
					warn("Invalid AT message: %s", reader.next);
					reader.next = NULL;
					continue;
				default:
					goto ioerror;
				}

			/* use predefined callback, otherwise get generic one */
			bool predefined_callback = false;
			if (r->handler != NULL && r->handler->type == reader.at.type &&
					strcmp(r->handler->command, reader.at.command) == 0) {
				callback = r->handler->callback;
				predefined_callback = true;
				r->handler = NULL;
			}
			else
				callback = rfcomm_get_callback(&reader.at);

			if (pfds[2].fd != -1 && !predefined_callback) {
				at_build(tmp, sizeof(tmp), reader.at.type,
						reader.at.command, reader.at.value);
				if (write(pfds[2].fd, tmp, strlen(tmp)) == -1)
					warn("Couldn't forward AT: %s", strerror(errno));
			}

			if (callback != NULL) {
				if ((ec = callback(r, &reader.at)) != ERROR_CODE_OK)
					goto ioerror;
			}
			else if (pfds[2].fd == -1) {
				warn("Unsupported AT message: %s: command:%s, value:%s",
						at_type2str(reader.at.type), reader.at.command, reader.at.value);
				if (reader.at.type != AT_TYPE_RESP)
					if ((ec = rfcomm_write_at(pfds[1].fd, AT_TYPE_RESP, NULL, "ERROR")) != ERROR_CODE_OK)
						goto ioerror;
			}

		}
		else if (pfds[1].revents & (POLLERR | POLLHUP)) {
			ec = ERROR_SYSTEM(ECONNRESET);
			goto ioerror;
		}

		if (pfds[2].revents & POLLIN) {
			/* read data from the external handler */

			ssize_t ret;
			while ((ret = read(pfds[2].fd, tmp, sizeof(tmp) - 1)) == -1 &&
					errno == EINTR)
				continue;

			if (ret <= 0) {
				ec = ERROR_SYSTEM(errno);
				goto ioerror_exthandler;
			}

			tmp[ret] = '\0';
			if ((ec = rfcomm_write_at(pfds[1].fd, AT_TYPE_RAW, tmp, NULL)) != ERROR_CODE_OK)
				goto ioerror;

		}
		else if (pfds[2].revents & (POLLERR | POLLHUP)) {
			ec = ERROR_SYSTEM(ECONNRESET);
			goto ioerror_exthandler;
		}

		continue;

ioerror_exthandler:
		if (ec != ERROR_CODE_OK)
			error("AT handler IO error: %s", error_code_strerror(ec));
		close(r->handler_fd);
		r->handler_fd = -1;
		continue;

ioerror:
		switch (ec) {
		case ERROR_SYSTEM(ECONNABORTED):
		case ERROR_SYSTEM(ECONNRESET):
		case ERROR_SYSTEM(ENOTCONN):
		case ERROR_SYSTEM(ETIMEDOUT):
		case ERROR_SYSTEM(EPIPE):
			/* exit the thread upon socket disconnection */
			debug("RFCOMM disconnected: %s", error_code_strerror(ec));
			goto fail;
		default:
			error("RFCOMM IO error: %s", error_code_strerror(ec));
		}
	}

fail:
	pthread_cleanup_pop(1);
	return NULL;
}

struct ba_rfcomm *ba_rfcomm_new(struct ba_transport *sco, int fd) {

	struct ba_rfcomm *r;
	int err;

	if ((r = calloc(1, sizeof(*r))) == NULL)
		return NULL;

	r->fd = fd;
	r->sig_fd[0] = -1;
	r->sig_fd[1] = -1;
	r->handler_fd = -1;
	r->thread = config.main_thread;
	r->state = HFP_DISCONNECTED;
	r->state_prev = HFP_DISCONNECTED;
	r->codec_id = HFP_CODEC_UNDEFINED;
	r->sco = ba_transport_ref(sco);
	r->link_lost_quirk = true;

	/* Initialize HFP feature masks and codec flags. Values for the remote
	 * device will be set during the SLC establishment. */

	if (sco->profile & BA_TRANSPORT_PROFILE_HFP_AG)
		r->ag_features = ba_adapter_get_hfp_features_ag(sco->d->a);
	if (sco->profile & BA_TRANSPORT_PROFILE_HFP_HF)
		r->hf_features = ba_adapter_get_hfp_features_hf(sco->d->a);

	/* HSP does not support codec negotiation, so we can set the codec
	 * flag right away. */
	if (sco->profile & BA_TRANSPORT_PROFILE_MASK_HSP) {
		r->ag_codecs.cvsd = true;
		r->hf_codecs.cvsd = true;
	}

	if (sco->profile & BA_TRANSPORT_PROFILE_HFP_AG) {
		if (config.hfp.codecs.cvsd)
			r->ag_codecs.cvsd = true;
#if ENABLE_MSBC
		if (config.hfp.codecs.msbc && r->ag_features & HFP_AG_FEAT_ESCO)
			r->ag_codecs.msbc = true;
#endif
#if ENABLE_LC3_SWB
		if (config.hfp.codecs.lc3_swb && r->ag_features & HFP_AG_FEAT_ESCO)
			r->ag_codecs.lc3_swb = true;
#endif
	}

	if (sco->profile & BA_TRANSPORT_PROFILE_HFP_HF) {

		char *ptr = r->hf_bac_bcs_string;

		if (config.hfp.codecs.cvsd) {
			ptr += sprintf(ptr, "%u", HFP_CODEC_CVSD);
			r->hf_codecs.cvsd = true;
		}

#if ENABLE_MSBC
		if (config.hfp.codecs.msbc && r->hf_features & HFP_HF_FEAT_ESCO) {
			const bool first = ptr == r->hf_bac_bcs_string;
			ptr += sprintf(ptr, "%s%u", first ? "" : ",", HFP_CODEC_MSBC);
			r->hf_codecs.msbc = true;
		}
#endif
#if ENABLE_LC3_SWB
		if (config.hfp.codecs.lc3_swb && r->hf_features & HFP_HF_FEAT_ESCO) {
			const bool first = ptr == r->hf_bac_bcs_string;
			ptr += sprintf(ptr, "%s%u", first ? "" : ",", HFP_CODEC_LC3_SWB);
			r->hf_codecs.lc3_swb = true;
		}
#endif

	}

	/* By default, all indicators are enabled. */
	memset(&r->hfp_ind_state, 1, sizeof(r->hfp_ind_state));

	/* Initialize data used for volume gain synchronization. */
	r->gain_mic = ba_transport_pcm_volume_level_to_range(
			sco->sco.pcm_mic.volume[0].level, HFP_VOLUME_GAIN_MAX);
	r->gain_spk = ba_transport_pcm_volume_level_to_range(
			sco->sco.pcm_spk.volume[0].level, HFP_VOLUME_GAIN_MAX);

	if (pipe(r->sig_fd) == -1)
		goto fail;

	pthread_cond_init(&r->codec_selection_cond, NULL);

	if ((err = pthread_create(&r->thread, NULL, PTHREAD_FUNC(rfcomm_thread), r)) != 0) {
		error("Couldn't create RFCOMM thread: %s", strerror(err));
		r->thread = config.main_thread;
		goto fail;
	}

	const char *name = "ba-rfcomm";
	pthread_setname_np(r->thread, name);
	debug("Created new RFCOMM thread [%s]: %s",
			name, ba_transport_debug_name(sco));

	r->ba_dbus_path = g_strdup_printf("%s/rfcomm", sco->d->ba_dbus_path);
	bluealsa_dbus_rfcomm_register(r);

	return r;

fail:
	err = errno;
	ba_rfcomm_destroy(r);
	errno = err;
	return NULL;
}

void ba_rfcomm_destroy(struct ba_rfcomm *r) {

	int err;

	/* Disable link lost quirk, because we don't want
	 * any interference during the destroy procedure. */
	r->link_lost_quirk = false;

	/* Remove D-Bus interfaces, so no one will access
	 * RFCOMM thread during the destroy procedure. */
	bluealsa_dbus_rfcomm_unregister(r);

	if (!pthread_equal(r->thread, config.main_thread)) {
		if (!pthread_equal(r->thread, pthread_self())) {
			if ((err = pthread_cancel(r->thread)) != 0 && err != ESRCH)
				warn("Couldn't cancel RFCOMM thread: %s", strerror(err));
			if ((err = pthread_join(r->thread, NULL)) != 0)
				warn("Couldn't join RFCOMM thread: %s", strerror(err));
		}
		else {
			/* It seems that the thread is being destroyed by the link lost
			 * quirk. Detach itself so we will not leak resources. */
			pthread_detach(r->thread);
		}
	}

	if (r->handler_fd != -1)
		close(r->handler_fd);

	if (r->sco != NULL)
		ba_transport_unref(r->sco);

	if (r->sig_fd[0] != -1)
		close(r->sig_fd[0]);
	if (r->sig_fd[1] != -1)
		close(r->sig_fd[1]);

	if (r->ba_dbus_path != NULL)
		g_free(r->ba_dbus_path);

	pthread_cond_destroy(&r->codec_selection_cond);

	free(r);
}

int ba_rfcomm_send_signal(struct ba_rfcomm *r, enum ba_rfcomm_signal sig) {
	return write(r->sig_fd[1], &sig, sizeof(sig));
}
