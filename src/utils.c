/*
 * BlueALSA - utils.c
 * Copyright (c) 2016-2019 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "utils.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <bluetooth/bluetooth.h>

#if ENABLE_LDAC
# include "ldacBT.h"
#endif

#include "a2dp-codecs.h"
#include "hfp.h"
#include "shared/defs.h"
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
 * Extract HCI device ID from the BlueZ D-Bus object path.
 *
 * @param path BlueZ D-Bus object path.
 * @return On success this function returns ID of the HCI device.
 *   Otherwise, -1 is returned. */
int g_dbus_bluez_object_path_to_hci_dev_id(const char *path) {
	if ((path = strstr(path, "/hci")) == NULL || path[4] == '\0')
		return -1;
	return atoi(&path[4]);
}

/**
 * Extract BT address from the BlueZ D-Bus object path.
 *
 * @param path BlueZ D-Bus object path.
 * @param addr Address where the parsed BT address will be stored.
 * @return On success this function returns pointer to the BT address. On
 *   error, NULL is returned. */
bdaddr_t *g_dbus_bluez_object_path_to_bdaddr(const char *path, bdaddr_t *addr) {

	char *tmp, *p;

	if ((path = strstr(path, "/dev_")) == NULL)
		return NULL;
	if ((tmp = strdup(path + 5)) == NULL)
		return NULL;

	for (p = tmp; *p != '\0'; p++)
		if (*p == '_')
			*p = ':';
		else if (*p == '/')
			*p = '\0';

	if (str2ba(tmp, addr) == -1)
		addr = NULL;

	free(tmp);
	return addr;
}

/**
 * Get BlueZ D-Bus object path for given transport type.
 *
 * @param type Transport type structure.
 * @return This function returns BlueZ D-Bus object path. */
const char *g_dbus_transport_type_to_bluez_object_path(struct ba_transport_type type) {
	switch (type.profile) {
	case BA_TRANSPORT_PROFILE_A2DP_SOURCE:
		switch (type.codec) {
		case A2DP_CODEC_SBC:
			return "/A2DP/SBC/Source";
#if ENABLE_MPEG
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
#if ENABLE_APTX_HD
		case A2DP_CODEC_VENDOR_APTX_HD:
			return "/A2DP/APTXHD/Source";
#endif
#if ENABLE_LDAC
		case A2DP_CODEC_VENDOR_LDAC:
			return "/A2DP/LDAC/Source";
#endif
		default:
			warn("Unsupported A2DP codec: %#x", type.codec);
			return "/A2DP/Source";
		}
	case BA_TRANSPORT_PROFILE_A2DP_SINK:
		switch (type.codec) {
		case A2DP_CODEC_SBC:
			return "/A2DP/SBC/Sink";
#if ENABLE_MPEG
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
#if ENABLE_APTX_HD
		case A2DP_CODEC_VENDOR_APTX_HD:
			return "/A2DP/APTXHD/Sink";
#endif
#if ENABLE_LDAC
		case A2DP_CODEC_VENDOR_LDAC:
			return "/A2DP/LDAC/Sink";
#endif
		default:
			warn("Unsupported A2DP codec: %#x", type.codec);
			return "/A2DP/Sink";
		}
	case BA_TRANSPORT_PROFILE_HFP_HF:
		return "/HFP/HandsFree";
	case BA_TRANSPORT_PROFILE_HFP_AG:
		return "/HFP/AudioGateway";
	case BA_TRANSPORT_PROFILE_HSP_HS:
		return "/HSP/Headset";
	case BA_TRANSPORT_PROFILE_HSP_AG:
		return "/HSP/AudioGateway";
	}
	return "/";
}

/**
 * Get managed objects of a given D-Bus service. */
GVariantIter *g_dbus_get_managed_objects(GDBusConnection *conn,
		const char *name, const char *path, GError **error) {

	GDBusMessage *msg = NULL, *rep = NULL;
	GVariantIter *objects = NULL;

	msg = g_dbus_message_new_method_call(name, path,
			"org.freedesktop.DBus.ObjectManager", "GetManagedObjects");

	if ((rep = g_dbus_connection_send_message_with_reply_sync(conn, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, error)) == NULL)
		goto fail;

	if (g_dbus_message_get_message_type(rep) == G_DBUS_MESSAGE_TYPE_ERROR) {
		g_dbus_message_to_gerror(rep, error);
		goto fail;
	}

	g_variant_get(g_dbus_message_get_body(rep), "(a{oa{sa{sv}}})", &objects);

fail:

	if (msg != NULL)
		g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);

	return objects;
}

/**
 * Get a property of a given D-Bus interface.
 *
 * @param conn D-Bus connection handler.
 * @param service Valid D-Bus service name.
 * @param path Valid D-Bus object path.
 * @param interface Interface with the given property.
 * @param property The property name.
 * @param error NULL GError pointer.
 * @return On success this function returns variant containing property value.
 *   Otherwise, NULL is returned. */
GVariant *g_dbus_get_property(GDBusConnection *conn, const char *service,
		const char *path, const char *interface, const char *property,
		GError **error) {

	GDBusMessage *msg = NULL, *rep = NULL;
	GVariant *value = NULL;

	msg = g_dbus_message_new_method_call(service, path, "org.freedesktop.DBus.Properties", "Get");
	g_dbus_message_set_body(msg, g_variant_new("(ss)", interface, property));

	if ((rep = g_dbus_connection_send_message_with_reply_sync(conn, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, error)) == NULL)
		goto fail;

	if (g_dbus_message_get_message_type(rep) == G_DBUS_MESSAGE_TYPE_ERROR) {
		g_dbus_message_to_gerror(rep, error);
		goto fail;
	}

	g_variant_get(g_dbus_message_get_body(rep), "(v)", &value);

fail:

	if (msg != NULL)
		g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);

	return value;
}

/**
 * Set a property of a given D-Bus interface.
 *
 * @param conn D-Bus connection handler.
 * @param service Valid D-Bus service name.
 * @param path Valid D-Bus object path.
 * @param interface Interface with the given property.
 * @param property The property name.
 * @param value Variant containing property value.
 * @param error NULL GError pointer.
 * @return On success this function returns true. */
bool g_dbus_set_property(GDBusConnection *conn, const char *service,
		const char *path, const char *interface, const char *property,
		const GVariant *value, GError **error) {

	GDBusMessage *msg = NULL, *rep = NULL;

	msg = g_dbus_message_new_method_call(service, path, "org.freedesktop.DBus.Properties", "Set");
	g_dbus_message_set_body(msg, g_variant_new("(ssv)", interface, property, value));

	if ((rep = g_dbus_connection_send_message_with_reply_sync(conn, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, error)) == NULL)
		goto fail;

	if (g_dbus_message_get_message_type(rep) == G_DBUS_MESSAGE_TYPE_ERROR) {
		g_dbus_message_to_gerror(rep, error);
		goto fail;
	}

fail:

	if (msg != NULL)
		g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);

	return error == NULL;
}

/**
 * Sanitize D-Bus object path.
 *
 * @param path D-Bus object path.
 * @return Pointer to the object path string. */
char *g_variant_sanitize_object_path(char *path) {

	char *tmp = path - 1;

	while (*(++tmp) != '\0')
		if (!(*tmp == '/' || isalnum(*tmp)))
			*tmp = '_';

	return path;
}

/**
 * Convenience wrapper around g_variant_is_of_type().
 *
 * @param value Variant for validation.
 * @param type Expected variant type.
 * @param name Variant name for logging.
 * @return If variant matches type, this function returns true. */
bool g_variant_validate_value(GVariant *value, const GVariantType *type,
		const char *name) {
	if (g_variant_is_of_type(value, type))
		return true;
	warn("Invalid variant type: %s: %s != %s", name,
			g_variant_get_type_string(value), (const char *)type);
	return false;
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

/**
 * Convert Bluetooth A2DP codec into a human-readable string.
 *
 * @param codec Bluetooth A2DP audio codec.
 * @return Human-readable string. */
const char *bluetooth_a2dp_codec_to_string(uint16_t codec) {
	switch (codec) {
	case A2DP_CODEC_SBC:
		return "SBC";
#if ENABLE_MPEG
	case A2DP_CODEC_MPEG12:
		return "MPEG";
#endif
#if ENABLE_AAC
	case A2DP_CODEC_MPEG24:
		return "AAC";
#endif
#if ENABLE_APTX
	case A2DP_CODEC_VENDOR_APTX:
		return "APT-X";
#endif
#if ENABLE_APTX_HD
	case A2DP_CODEC_VENDOR_APTX_HD:
		return "APT-X HD";
#endif
#if ENABLE_LDAC
	case A2DP_CODEC_VENDOR_LDAC:
		return "LDAC";
#endif
	}
	debug("Unknown codec: %#x", codec);
	return "N/A";
}

/**
 * Convert BlueALSA transport type into a human-readable string.
 *
 * @param type Transport type structure.
 * @return Human-readable string. */
const char *ba_transport_type_to_string(struct ba_transport_type type) {
	switch (type.profile) {
	case BA_TRANSPORT_PROFILE_A2DP_SOURCE:
		switch (type.codec) {
		case A2DP_CODEC_SBC:
			return "A2DP Source (SBC)";
#if ENABLE_MPEG
		case A2DP_CODEC_MPEG12:
			return "A2DP Source (MPEG)";
#endif
#if ENABLE_AAC
		case A2DP_CODEC_MPEG24:
			return "A2DP Source (AAC)";
#endif
#if ENABLE_APTX
		case A2DP_CODEC_VENDOR_APTX:
			return "A2DP Source (APT-X)";
#endif
#if ENABLE_APTX_HD
		case A2DP_CODEC_VENDOR_APTX_HD:
			return "A2DP Source (APT-X HD)";
#endif
#if ENABLE_LDAC
		case A2DP_CODEC_VENDOR_LDAC:
			return "A2DP Source (LDAC)";
#endif
		default:
			return "A2DP Source";
		}
	case BA_TRANSPORT_PROFILE_A2DP_SINK:
		switch (type.codec) {
		case A2DP_CODEC_SBC:
			return "A2DP Sink (SBC)";
#if ENABLE_MPEG
		case A2DP_CODEC_MPEG12:
			return "A2DP Sink (MPEG)";
#endif
#if ENABLE_AAC
		case A2DP_CODEC_MPEG24:
			return "A2DP Sink (AAC)";
#endif
#if ENABLE_APTX
		case A2DP_CODEC_VENDOR_APTX:
			return "A2DP Sink (APT-X)";
#endif
#if ENABLE_APTX_HD
		case A2DP_CODEC_VENDOR_APTX_HD:
			return "A2DP Sink (APT-X HD)";
#endif
#if ENABLE_LDAC
		case A2DP_CODEC_VENDOR_LDAC:
			return "A2DP Sink (LDAC)";
#endif
		default:
			return "A2DP Sink";
		}
	case BA_TRANSPORT_PROFILE_HFP_HF:
		switch (type.codec) {
		case HFP_CODEC_CVSD:
			return "HFP Hands-Free (CVSD)";
		case HFP_CODEC_MSBC:
			return "HFP Hands-Free (mSBC)";
		default:
			return "HFP Hands-Free";
		}
	case BA_TRANSPORT_PROFILE_HFP_AG:
		switch (type.codec) {
		case HFP_CODEC_CVSD:
			return "HFP Audio Gateway (CVSD)";
		case HFP_CODEC_MSBC:
			return "HFP Audio Gateway (mSBC)";
		default:
			return "HFP Audio Gateway";
		}
	case BA_TRANSPORT_PROFILE_HSP_HS:
		return "HSP Headset";
	case BA_TRANSPORT_PROFILE_HSP_AG:
		return "HSP Audio Gateway";
	case BA_TRANSPORT_PROFILE_RFCOMM | BA_TRANSPORT_PROFILE_HFP_HF:
		return "RFCOMM: HFP Hands-Free";
	case BA_TRANSPORT_PROFILE_RFCOMM | BA_TRANSPORT_PROFILE_HFP_AG:
		return "RFCOMM: HFP Audio Gateway";
	case BA_TRANSPORT_PROFILE_RFCOMM | BA_TRANSPORT_PROFILE_HSP_HS:
		return "RFCOMM: HSP Headset";
	case BA_TRANSPORT_PROFILE_RFCOMM | BA_TRANSPORT_PROFILE_HSP_AG:
		return "RFCOMM: HSP Audio Gateway";
	}
	debug("Unknown transport type: %#x %#x", type.profile, type.codec);
	return "N/A";
}

#if ENABLE_MP3LAME
/**
 * Get maximum possible bit-rate for the given bit-rate mask.
 *
 * @param mask MPEG-1 layer III bit-rate mask.
 * @return Bit-rate in kilobits per second. */
int a2dp_mpeg1_mp3_get_max_bitrate(uint16_t mask) {

	static int bitrates[] = { 320, 256, 224, 192, 160, 128, 112, 96, 80, 64, 56, 48, 40, 32 };
	size_t i = 0;

	while (i < ARRAYSIZE(bitrates)) {
		if (mask & (1 << (14 - i)))
			return bitrates[i];
		i++;
	}

	return -1;
}
#endif

#if ENABLE_MP3LAME
/**
 * Get string representation of LAME error code.
 *
 * @param error LAME encoder error code.
 * @return Human-readable string. */
const char *lame_encode_strerror(int err) {
	switch (err) {
	case -1:
		return "Too small output buffer";
	case -2:
		return "Out of memory";
	case -3:
		return "Params not initialized";
	case -4:
		return "Psycho acoustic error";
	default:
		debug("Unknown error code: %#x", err);
		return "Unknown error";
	}
}
#endif

#if ENABLE_AAC
/**
 * Get string representation of the FDK-AAC decoder error code.
 *
 * @param err FDK-AAC decoder error code.
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
 * @param err FDK-AAC encoder error code.
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

#if ENABLE_LDAC
/**
 * Get string representation of the LDAC error code.
 *
 * @param err LDAC error code.
 * @return Human-readable string. */
const char *ldacBT_strerror(int err) {
	switch (LDACBT_API_ERR(err)) {
	case LDACBT_ERR_NONE:
		return "Success";
	case LDACBT_ERR_ASSERT_SAMPLING_FREQ:
	case LDACBT_ERR_ASSERT_SUP_SAMPLING_FREQ:
	case LDACBT_ERR_CHECK_SAMPLING_FREQ:
		return "Invalid sample rate";
	case LDACBT_ERR_ASSERT_CHANNEL_CONFIG:
	case LDACBT_ERR_CHECK_CHANNEL_CONFIG:
		return "Invalid channel config";
	case LDACBT_ERR_ASSERT_FRAME_LENGTH:
	case LDACBT_ERR_ASSERT_SUP_FRAME_LENGTH:
	case LDACBT_ERR_ASSERT_FRAME_STATUS:
		return "Invalid frame status";
	case LDACBT_ERR_ASSERT_NSHIFT:
		return "Invalid N-shift";
	case LDACBT_ERR_ASSERT_CHANNEL_MODE:
		return "Invalid channel mode";
	case LDACBT_ERR_ALTER_EQMID_LIMITED:
		return "EQMID limited";
	case LDACBT_ERR_HANDLE_NOT_INIT:
		return "Invalid handle";
	case LDACBT_ERR_ILL_EQMID:
		return "Unsupported EQMID";
	case LDACBT_ERR_ILL_SAMPLING_FREQ:
		return "Unsupported sample rate";
	case LDACBT_ERR_ILL_NUM_CHANNEL:
		return "Unsupported channels";
	case LDACBT_ERR_ILL_MTU_SIZE:
		return "Unsupported MTU";
	default:
		debug("Unknown error code: %#x", err);
		return "Unknown error";
	}
}
#endif
