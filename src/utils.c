/*
 * BlueALSA - utils.c
 * Copyright (c) 2016 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "utils.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/sco.h>

#include "a2dp-codecs.h"
#include "bluez.h"
#include "log.h"
#include "transport.h"


/**
 * Calculate the optimum bitpool for given parameters.
 *
 * @param freq Sampling frequency.
 * @param model Channel mode.
 * @return Coded SBC bitpool. */
int a2dp_sbc_default_bitpool(int freq, int mode) {
	switch (freq) {
	case SBC_SAMPLING_FREQ_16000:
	case SBC_SAMPLING_FREQ_32000:
		return 53;
	case SBC_SAMPLING_FREQ_44100:
		switch (mode) {
		case SBC_CHANNEL_MODE_MONO:
		case SBC_CHANNEL_MODE_DUAL_CHANNEL:
			return 31;
		case SBC_CHANNEL_MODE_STEREO:
		case SBC_CHANNEL_MODE_JOINT_STEREO:
			return 53;
		default:
			warn("Invalid channel mode: %u", mode);
			return 53;
		}
	case SBC_SAMPLING_FREQ_48000:
		switch (mode) {
		case SBC_CHANNEL_MODE_MONO:
		case SBC_CHANNEL_MODE_DUAL_CHANNEL:
			return 29;
		case SBC_CHANNEL_MODE_STEREO:
		case SBC_CHANNEL_MODE_JOINT_STEREO:
			return 51;
		default:
			warn("Invalid channel mode: %u", mode);
			return 51;
		}
	default:
		warn("Invalid sampling freq: %u", freq);
		return 53;
	}
}

/**
 * Get the list of all available HCI controllers.
 *
 * @param di The address to the device info structure pointer, where the list
 *	 of all available devices will be stored. Allocated memory should be freed
 *	 with the free().
 * @param num The address, where the number of initialized device structures
 *	 will be stored.
 * @return On success this function returns 0. Otherwise, -1 is returned and
 *	 errno is set to indicate the error. */
int hci_devlist(struct hci_dev_info **di, int *num) {

	int i;

	if ((*di = malloc(HCI_MAX_DEV * sizeof(**di))) == NULL)
		return -1;

	for (i = *num = 0; i < HCI_MAX_DEV; i++)
		if (hci_devinfo(i, &(*di)[*num]) == 0)
			(*num)++;

	return 0;
}

/**
 * Open SCO link for given Bluetooth device.
 *
 * @param di The address to the HCI device info structure for which the SCO
 *   link should be established.
 * @param ba Pointer to the Bluetooth address structure for a target device.
 * @return On success this function returns socket file descriptor. Otherwise,
 *   -1 is returned and errno is set to indicate the error. */
int hci_open_sco(const struct hci_dev_info *di, const bdaddr_t *ba) {

	struct sockaddr_sco addr_hci = {
		.sco_family = AF_BLUETOOTH,
		.sco_bdaddr = di->bdaddr,
	};
	struct sockaddr_sco addr_dev = {
		.sco_family = AF_BLUETOOTH,
		.sco_bdaddr = *ba,
	};
	int dd, err;

	if ((dd = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_SCO)) == -1)
		return -1;
	if (bind(dd, (struct sockaddr *)&addr_hci, sizeof(addr_hci)) == -1)
		goto fail;
	if (connect(dd, (struct sockaddr *)&addr_dev, sizeof(addr_dev)) == -1)
		goto fail;

	return dd;

fail:
	err = errno;
	close(dd);
	errno = err;
	return -1;
}

/**
 * Convert BlueZ D-Bus object path into a transport profile.
 *
 * @param path BlueZ D-Bus object path.
 * @return On success this function returns transport profile. If object
 *   path cannot be recognize, 0 is returned. */
int g_dbus_object_path_to_profile(const char *path) {

	static GHashTable *profiles = NULL;

	/* initialize profile hash table */
	if (profiles == NULL) {

		size_t i;
		const struct profile_data {
			char *endpoint;
			unsigned int profile;
		} data[] = {
			{ BLUEZ_ENDPOINT_A2DP_SBC_SOURCE, TRANSPORT_PROFILE_A2DP_SOURCE },
			{ BLUEZ_ENDPOINT_A2DP_SBC_SINK, TRANSPORT_PROFILE_A2DP_SINK },
			{ BLUEZ_ENDPOINT_A2DP_MPEG12_SOURCE, TRANSPORT_PROFILE_A2DP_SOURCE },
			{ BLUEZ_ENDPOINT_A2DP_MPEG12_SINK, TRANSPORT_PROFILE_A2DP_SINK },
			{ BLUEZ_ENDPOINT_A2DP_MPEG24_SOURCE, TRANSPORT_PROFILE_A2DP_SOURCE },
			{ BLUEZ_ENDPOINT_A2DP_MPEG24_SINK, TRANSPORT_PROFILE_A2DP_SINK },
			{ BLUEZ_ENDPOINT_A2DP_ATRAC_SOURCE, TRANSPORT_PROFILE_A2DP_SOURCE },
			{ BLUEZ_ENDPOINT_A2DP_ATRAC_SINK, TRANSPORT_PROFILE_A2DP_SINK },
			{ BLUEZ_PROFILE_HSP_HS, TRANSPORT_PROFILE_HSP_HS },
			{ BLUEZ_PROFILE_HSP_AG, TRANSPORT_PROFILE_HSP_AG },
			{ BLUEZ_PROFILE_HFP_HF, TRANSPORT_PROFILE_HFP_HF },
			{ BLUEZ_PROFILE_HFP_AG, TRANSPORT_PROFILE_HFP_AG },
		};

		profiles = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, NULL);
		for (i = 0; i < sizeof(data) / sizeof(struct profile_data); i++)
			g_hash_table_insert(profiles, data[i].endpoint, GINT_TO_POINTER(data[i].profile));

	}

	return GPOINTER_TO_INT(g_hash_table_lookup(profiles, path));
}

/**
 * Convert BlueZ D-Bus device path into a bdaddr_t structure.
 *
 * @param path BlueZ D-Bus device path.
 * @param addr Address where the parsed address will be stored.
 * @return On success this function returns 0. Otherwise, -1 is returned. */
int g_dbus_device_path_to_bdaddr(const char *path, bdaddr_t *addr) {

	char *tmp, *p;
	int ret;

	if ((path = strrchr(path, '/')) == NULL)
		return -1;
	if ((path = strstr(path, "dev_")) == NULL)
		return -1;
	if ((tmp = strdup(path + 4)) == NULL)
		return -1;

	for (p = tmp; *p != '\0'; p++)
		if (*p == '_')
			*p = ':';

	ret = str2ba(tmp, addr);

	free(tmp);
	return ret;
}

/**
 * Get a property of a given D-Bus interface.
 *
 * @param conn D-Bus connection handler.
 * @param name Valid D-Bus name or NULL.
 * @param path Valid D-Bus object path.
 * @param interface Interface with the given property.
 * @param property The property name.
 * @return On success this function returns variant containing property value.
 *   Otherwise, NULL is returned. */
GVariant *g_dbus_get_property(GDBusConnection *conn, const char *name,
		const char *path, const char *interface, const char *property) {

	GDBusMessage *msg = NULL, *rep = NULL;
	GVariant *value = NULL;
	GError *err = NULL;

	msg = g_dbus_message_new_method_call(name, path, "org.freedesktop.DBus.Properties", "Get");
	g_dbus_message_set_body(msg, g_variant_new("(ss)", interface, property, NULL));

	if ((rep = g_dbus_connection_send_message_with_reply_sync(conn, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, &err)) == NULL)
		goto fail;

	if (g_dbus_message_get_message_type(rep) == G_DBUS_MESSAGE_TYPE_ERROR) {
		g_dbus_message_to_gerror(rep, &err);
		goto fail;
	}

	g_variant_get(g_dbus_message_get_body(rep), "(v)", &value);

fail:

	if (msg != NULL)
		g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);
	if (err != NULL) {
		warn("Couldn't get property: %s", err->message);
		g_error_free(err);
	}

	return value;
}

/**
 * Convert Bluetooth profile into a human-readable string.
 *
 * @param profile Bluetooth profile.
 * @param codec Bluetooth profile audio codec.
 * @return Human-readable string. */
const char *bluetooth_profile_to_string(uint8_t profile, uint8_t codec) {
	switch (profile) {
	case TRANSPORT_PROFILE_A2DP_SOURCE:
		switch (codec) {
		case A2DP_CODEC_SBC:
			return "A2DP Source (SBC)";
		case A2DP_CODEC_MPEG12:
			return "A2DP Source (MP3)";
		case A2DP_CODEC_MPEG24:
			return "A2DP Source (AAC)";
		}
		return "A2DP Source";
	case TRANSPORT_PROFILE_A2DP_SINK:
		switch (codec) {
		case A2DP_CODEC_SBC:
			return "A2DP Sink (SBC)";
		case A2DP_CODEC_MPEG12:
			return "A2DP Sink (MP3)";
		case A2DP_CODEC_MPEG24:
			return "A2DP Sink (AAC)";
		}
		return "A2DP Sink";
	case TRANSPORT_PROFILE_HSP_HS:
		return "HSP Headset";
	case TRANSPORT_PROFILE_HSP_AG:
		return "HSP Audio Gateway";
	case TRANSPORT_PROFILE_HFP_HF:
		return "HFP Hands-Free";
	case TRANSPORT_PROFILE_HFP_AG:
		return "HFP Audio Gateway";
	default:
		return "N/A";
	}
}

/**
 * Convert Bluetooth address into a human-readable string.
 *
 * This function returns statically allocated buffer. It is not by any means
 * thread safe. This function should be used for debugging purposes only.
 *
 * For debugging purposes, one could use the batostr() function provided by
 * the bluez library. However, this function converts the Bluetooth address
 * to the string with a incorrect (reversed) bytes order...
 *
 * @param ba Pointer to the Bluetooth address structure.
 * @return On success this function returns statically allocated buffer with
 *   human-readable Bluetooth address. On error, it returns NULL. */
const char *batostr_(const bdaddr_t *ba) {
	static char addr[18];
	if (ba2str(ba, addr) > 0)
		return addr;
	return NULL;
}

/**
 * Mute PCM signal stored in the buffer.
 *
 * @param buffer Address to the buffer where the PCM signal is stored.
 * @param size The number of samples in the buffer. */
void snd_pcm_mute_s16le(int16_t *buffer, size_t size) {
	memset(buffer, 0, size * sizeof(*buffer));
}

/**
 * Scale PCM signal stored in the buffer.
 *
 * @param buffer Address to the buffer where the PCM signal is stored.
 * @param size The number of samples in the buffer.
 * @param scale The scaling factor - 100 is a neutral value. */
void snd_pcm_scale_s16le(int16_t *buffer, size_t size, int scale) {
	while (size--)
		buffer[size] = buffer[size] * scale / 100;
}

/**
 * Calculate time difference.
 *
 * @param ts1 Address to the timespec structure providing t1 time point.
 * @param ts2 Address to the timespec structure providing t2 time point.
 * @param ts Address to the timespec structure where the absolute time
 *   difference will be stored.
 * @return This function returns an integer less than, equal to, or greater
 *   than zero, if t2 time point is found to be, respectively, less than,
 *   equal to, or greater than the t1 time point.*/
int difftimespec(const struct timespec *ts1, const struct timespec *ts2,
		struct timespec *ts) {

	const struct timespec _ts1 = *ts1;
	const struct timespec _ts2 = *ts2;

	if (_ts1.tv_sec == _ts2.tv_sec) {
		ts->tv_sec = 0;
		ts->tv_nsec = labs(_ts2.tv_nsec - _ts1.tv_nsec);
		return _ts2.tv_nsec > _ts1.tv_nsec ? 1 : -ts->tv_nsec;
	}

	if (_ts1.tv_sec < _ts2.tv_sec) {
		if (_ts1.tv_nsec <= _ts2.tv_nsec) {
			ts->tv_sec = _ts2.tv_sec - _ts1.tv_sec;
			ts->tv_nsec = _ts2.tv_nsec - _ts1.tv_nsec;
		}
		else {
			ts->tv_sec = _ts2.tv_sec - 1 - _ts1.tv_sec;
			ts->tv_nsec = _ts2.tv_nsec + 1000000000 - _ts1.tv_nsec;
		}
		return 1;
	}

	if (_ts1.tv_nsec >= _ts2.tv_nsec) {
		ts->tv_sec = _ts1.tv_sec - _ts2.tv_sec;
		ts->tv_nsec = _ts1.tv_nsec - _ts2.tv_nsec;
	}
	else {
		ts->tv_sec = _ts1.tv_sec - 1 - _ts2.tv_sec;
		ts->tv_nsec = _ts1.tv_nsec + 1000000000 - _ts2.tv_nsec;
	}
	return -1;
}

#if ENABLE_AAC
/**
 * Get string representation of the FDK-AAC decoder error code.
 *
 * @param error FDK-AAC decoder error code.
 * @return Human-readable string. */
const char *aacdec_strerror(AAC_DECODER_ERROR err) {
	switch (err) {
	case AAC_DEC_OK:
		return "Success";
	case AAC_DEC_OUT_OF_MEMORY:
		return "Out of memory";
	case AAC_DEC_TRANSPORT_SYNC_ERROR:
		return "Transport sync error";
	case AAC_DEC_NOT_ENOUGH_BITS:
		return "Not enough bits";
	case AAC_DEC_INVALID_HANDLE:
		return "Invalid handle";
	case AAC_DEC_UNSUPPORTED_AOT:
		return "Unsupported AOT";
	case AAC_DEC_UNSUPPORTED_FORMAT:
		return "Unsupported format";
	case AAC_DEC_UNSUPPORTED_ER_FORMAT:
		return "Unsupported ER format";
	case AAC_DEC_UNSUPPORTED_EPCONFIG:
		return "Unsupported EP format";
	case AAC_DEC_UNSUPPORTED_MULTILAYER:
		return "Unsupported multilayer";
	case AAC_DEC_UNSUPPORTED_CHANNELCONFIG:
		return "Unsupported channels";
	case AAC_DEC_UNSUPPORTED_SAMPLINGRATE:
		return "Unsupported sample rate";
	case AAC_DEC_INVALID_SBR_CONFIG:
		return "Unsupported SBR";
	case AAC_DEC_SET_PARAM_FAIL:
		return "Unsupported parameter";
	case AAC_DEC_NEED_TO_RESTART:
		return "Restart required";
	case AAC_DEC_TRANSPORT_ERROR:
		return "Transport error";
	case AAC_DEC_PARSE_ERROR:
		return "Parse error";
	case AAC_DEC_UNSUPPORTED_EXTENSION_PAYLOAD:
		return "Unsupported extension payload";
	case AAC_DEC_DECODE_FRAME_ERROR:
		return "Bitstream corrupted";
	case AAC_DEC_CRC_ERROR:
		return "CRC mismatch";
	case AAC_DEC_INVALID_CODE_BOOK:
		return "Invalid codebook";
	case AAC_DEC_UNSUPPORTED_PREDICTION:
		return "Unsupported prediction";
	case AAC_DEC_UNSUPPORTED_CCE:
		return "Unsupported CCE";
	case AAC_DEC_UNSUPPORTED_LFE:
		return "Unsupported LFE";
	case AAC_DEC_UNSUPPORTED_GAIN_CONTROL_DATA:
		return "Unsupported gain control data";
	case AAC_DEC_UNSUPPORTED_SBA:
		return "Unsupported SBA";
	case AAC_DEC_TNS_READ_ERROR:
		return "TNS read error";
	case AAC_DEC_RVLC_ERROR:
		return "RVLC decode error";
	case AAC_DEC_ANC_DATA_ERROR:
		return "Ancillary data error";
	case AAC_DEC_TOO_SMALL_ANC_BUFFER:
		return "Too small ancillary buffer";
	case AAC_DEC_TOO_MANY_ANC_ELEMENTS:
		return "Too many ancillary elements";
	default:
		debug("Unknown error code: %x", err);
		return "Unknown error";
	}
}
#endif

#if ENABLE_AAC
/**
 * Get string representation of the FDK-AAC encoder error code.
 *
 * @param error FDK-AAC encoder error code.
 * @return Human-readable string. */
const char *aacenc_strerror(AACENC_ERROR err) {
	switch (err) {
	case AACENC_OK:
		return "Success";
	case AACENC_INVALID_HANDLE:
		return "Invalid handle";
	case AACENC_MEMORY_ERROR:
		return "Out of memory";
	case AACENC_UNSUPPORTED_PARAMETER:
		return "Unsupported parameter";
	case AACENC_INVALID_CONFIG:
		return "Invalid config";
	case AACENC_INIT_ERROR:
		return "Initialization error";
	case AACENC_INIT_AAC_ERROR:
		return "AAC library initialization error";
	case AACENC_INIT_SBR_ERROR:
		return "SBR library initialization error";
	case AACENC_INIT_TP_ERROR:
		return "Transport library initialization error";
	case AACENC_INIT_META_ERROR:
		return "Metadata library initialization error";
	case AACENC_ENCODE_ERROR:
		return "Encoding error";
	case AACENC_ENCODE_EOF:
		return "End of file";
	default:
		debug("Unknown error code: %x", err);
		return "Unknown error";
	}
}
#endif
