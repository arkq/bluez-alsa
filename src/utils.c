/*
 * BlueALSA - utils.c
 * Copyright (c) 2016-2017 Arkadiusz Bokowy
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
#include "shared/log.h"


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
 * @param transparent Use transparent mode for voice transmission.
 * @return On success this function returns socket file descriptor. Otherwise,
 *   -1 is returned and errno is set to indicate the error. */
int hci_open_sco(const struct hci_dev_info *di, const bdaddr_t *ba, bool transparent) {

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

	if (transparent) {
		struct bt_voice voice = {
			.setting = BT_VOICE_TRANSPARENT,
		};
		if (setsockopt(dd, SOL_BLUETOOTH, BT_VOICE, &voice, sizeof(voice)) == -1)
			goto fail;
	}

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
 * Get BlueZ D-Bus object path for given profile and codec.
 *
 * @param profile Bluetooth profile.
 * @param codec Bluetooth profile codec.
 * @return This function returns BlueZ D-Bus object path. */
const char *g_dbus_get_profile_object_path(enum bluetooth_profile profile, uint16_t codec) {
	switch (profile) {
	case BLUETOOTH_PROFILE_A2DP_SOURCE:
		switch (codec) {
		case A2DP_CODEC_SBC:
			return "/A2DP/SBC/Source";
#if ENABLE_MP3
		case A2DP_CODEC_MPEG12:
			return "/A2DP/MPEG12/Source";
#endif
#if ENABLE_AAC
		case A2DP_CODEC_MPEG24:
			return "/A2DP/MPEG24/Source";
#endif
#if ENABLE_APTX
		case A2DP_CODEC_VENDOR_APTX:
			return "/A2DP/APTX/Source";
#endif
		default:
			warn("Unsupported A2DP codec: %#x", codec);
			return "/A2DP/Source";
		}
	case BLUETOOTH_PROFILE_A2DP_SINK:
		switch (codec) {
		case A2DP_CODEC_SBC:
			return "/A2DP/SBC/Sink";
#if ENABLE_MP3
		case A2DP_CODEC_MPEG12:
			return "/A2DP/MPEG12/Sink";
#endif
#if ENABLE_AAC
		case A2DP_CODEC_MPEG24:
			return "/A2DP/MPEG24/Sink";
#endif
#if ENABLE_APTX
		case A2DP_CODEC_VENDOR_APTX:
			return "/A2DP/APTX/Sink";
#endif
		default:
			warn("Unsupported A2DP codec: %#x", codec);
			return "/A2DP/Sink";
		}
	case BLUETOOTH_PROFILE_HSP_HS:
		return "/HSP/Headset";
	case BLUETOOTH_PROFILE_HSP_AG:
		return "/HSP/AudioGateway";
	case BLUETOOTH_PROFILE_HFP_HF:
		return "/HFP/HandsFree";
	case BLUETOOTH_PROFILE_HFP_AG:
		return "/HFP/AudioGateway";
	case BLUETOOTH_PROFILE_NULL:
		break;
	}
	return "/";
}

/**
 * Convert BlueZ D-Bus object path into a Bluetooth profile.
 *
 * @param path BlueZ D-Bus object path.
 * @return On success this function returns Bluetooth profile. If object
 *   path cannot be recognize, NULL profile is returned. */
enum bluetooth_profile g_dbus_object_path_to_profile(const char *path) {
	if (strncmp(path, "/A2DP", 5) == 0) {
		if (strstr(path + 5, "/Source") != NULL)
			return BLUETOOTH_PROFILE_A2DP_SOURCE;
		if (strstr(path + 5, "/Sink") != NULL)
			return BLUETOOTH_PROFILE_A2DP_SINK;
	}
	if (strncmp(path, "/HSP", 4) == 0) {
		if (strcmp(path + 4, "/Headset") == 0)
			return BLUETOOTH_PROFILE_HSP_HS;
		if (strcmp(path + 4, "/AudioGateway") == 0)
			return BLUETOOTH_PROFILE_HSP_AG;
	}
	if (strncmp(path, "/HFP", 4) == 0) {
		if (strcmp(path + 4, "/HandsFree") == 0)
			return BLUETOOTH_PROFILE_HFP_HF;
		if (strcmp(path + 4, "/AudioGateway") == 0)
			return BLUETOOTH_PROFILE_HFP_AG;
	}
	return BLUETOOTH_PROFILE_NULL;
}

/**
 * Convert BlueZ D-Bus object path into a A2DP codec.
 *
 * Prior to the usage, make sure, that the path is for the A2DP profile.
 * To do so, use the g_dbus_object_path_to_profile() function.
 *
 * @param path BlueZ D-Bus object path.
 * @return On success this function returns Bluetooth audio codec. If object
 *   path cannot be recognize, vendor codec is returned. */
uint16_t g_dbus_object_path_to_a2dp_codec(const char *path) {
	if (strncmp(path + 5, "/SBC", 4) == 0)
		return A2DP_CODEC_SBC;
#if ENABLE_MP3
	if (strncmp(path + 5, "/MPEG12", 7) == 0)
		return A2DP_CODEC_MPEG12;
#endif
#if ENABLE_AAC
	if (strncmp(path + 5, "/MPEG24", 7) == 0)
		return A2DP_CODEC_MPEG24;
#endif
#if ENABLE_APTX
	if (strncmp(path + 5, "/APTX", 5) == 0)
		return A2DP_CODEC_VENDOR_APTX;
#endif
	return A2DP_CODEC_VENDOR;
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
	g_dbus_message_set_body(msg, g_variant_new("(ss)", interface, property));

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
 * Set a property of a given D-Bus interface.
 *
 * @param conn D-Bus connection handler.
 * @param name Valid D-Bus name or NULL.
 * @param path Valid D-Bus object path.
 * @param interface Interface with the given property.
 * @param property The property name.
 * @param value Variant containing property value.
 * @return On success this function returns TRUE. Otherwise, FALSE. */
gboolean g_dbus_set_property(GDBusConnection *conn, const char *name,
		const char *path, const char *interface, const char *property,
		const GVariant *value) {

	GDBusMessage *msg = NULL, *rep = NULL;
	GError *err = NULL;

	msg = g_dbus_message_new_method_call(name, path, "org.freedesktop.DBus.Properties", "Set");
	g_dbus_message_set_body(msg, g_variant_new("(ssv)", interface, property, value));

	if ((rep = g_dbus_connection_send_message_with_reply_sync(conn, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, &err)) == NULL)
		goto fail;

	if (g_dbus_message_get_message_type(rep) == G_DBUS_MESSAGE_TYPE_ERROR) {
		g_dbus_message_to_gerror(rep, &err);
		goto fail;
	}

fail:

	if (msg != NULL)
		g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);
	if (err != NULL) {
		warn("Couldn't set property: %s", err->message);
		g_error_free(err);
		return FALSE;
	}

	return TRUE;
}

/**
 * Convert Bluetooth profile into a human-readable string.
 *
 * @param profile Bluetooth profile.
 * @param codec Bluetooth profile audio codec.
 * @return Human-readable string. */
const char *bluetooth_profile_to_string(enum bluetooth_profile profile, uint16_t codec) {
	switch (profile) {
	case BLUETOOTH_PROFILE_NULL:
		return "N/A";
	case BLUETOOTH_PROFILE_A2DP_SOURCE:
		switch (codec) {
		case A2DP_CODEC_SBC:
			return "A2DP Source (SBC)";
#if ENABLE_MP3
		case A2DP_CODEC_MPEG12:
			return "A2DP Source (MP3)";
#endif
#if ENABLE_AAC
		case A2DP_CODEC_MPEG24:
			return "A2DP Source (AAC)";
#endif
#if ENABLE_APTX
		case A2DP_CODEC_VENDOR_APTX:
			return "A2DP Source (APT-X)";
#endif
		}
		return "A2DP Source";
	case BLUETOOTH_PROFILE_A2DP_SINK:
		switch (codec) {
		case A2DP_CODEC_SBC:
			return "A2DP Sink (SBC)";
#if ENABLE_MP3
		case A2DP_CODEC_MPEG12:
			return "A2DP Sink (MP3)";
#endif
#if ENABLE_AAC
		case A2DP_CODEC_MPEG24:
			return "A2DP Sink (AAC)";
#endif
#if ENABLE_APTX
		case A2DP_CODEC_VENDOR_APTX:
			return "A2DP Sink (APT-X)";
#endif
		}
		return "A2DP Sink";
	case BLUETOOTH_PROFILE_HSP_HS:
		return "HSP Headset";
	case BLUETOOTH_PROFILE_HSP_AG:
		return "HSP Audio Gateway";
	case BLUETOOTH_PROFILE_HFP_HF:
		return "HFP Hands-Free";
	case BLUETOOTH_PROFILE_HFP_AG:
		return "HFP Audio Gateway";
	}
	return "N/A";
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
 * Scale PCM signal stored in the buffer.
 *
 * Neutral value for scaling factor is 1.0. It is possible to increase
 * signal gain by using scaling factor values greater than 1, however
 * clipping will most certainly occur.
 *
 * @param buffer Address to the buffer where the PCM signal is stored.
 * @param size The number of samples in the buffer.
 * @param channels The number of channels in the buffer.
 * @param ch1_scale The scaling factor for 1st channel.
 * @param ch1_scale The scaling factor for 2nd channel. */
void snd_pcm_scale_s16le(int16_t *buffer, size_t size, int channels,
		double ch1_scale, double ch2_scale) {
	switch (channels) {
	case 1:
		if (ch1_scale != 1.0)
			while (size--)
				buffer[size] = buffer[size] * ch1_scale;
		break;
	case 2:
		if (ch1_scale != 1.0 || ch2_scale != 1.0)
			while (size--) {
				double scale = size % 2 == 0 ? ch1_scale : ch2_scale;
				buffer[size] = buffer[size] * scale;
			}
		break;
	}
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
		debug("Unknown error code: %#x", err);
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
		debug("Unknown error code: %#x", err);
		return "Unknown error";
	}
}
#endif
