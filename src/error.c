/*
 * BlueALSA - error.c
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "error.h"

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <string.h>

#if ENABLE_LDAC
# include <ldacBT.h>
#endif

#if ENABLE_LHDC
# include <lhdcBT_dec.h>
#endif

#include "shared/log.h"

#define ERROR_DOMAIN(err)   ((enum error_domain)((err) >> 28))
#define ERROR_CODE(err)     ((err) & 0x0FFFFFFF)

static const char * app_strerror(enum error_code err) {
	switch (err) {
	case ERROR_CODE_OK:
		return "Success";
	case ERROR_CODE_CONTINUE:
		return "Continue";
	case ERROR_CODE_NOT_FOUND:
		return "Not found";
	case ERROR_CODE_INVALID_SIZE:
		return "Invalid size";
	case ERROR_CODE_INVALID_STREAM:
		return "Invalid stream";
	case ERROR_CODE_MISSING_CAPABILITIES:
		return "Missing capabilities";
	case ERROR_CODE_A2DP_INVALID_CHANNELS:
		return "Invalid number of channels";
	case ERROR_CODE_A2DP_NOT_SUPPORTED_CHANNELS:
		return "Unsupported number of channels";
	case ERROR_CODE_A2DP_INVALID_CHANNEL_MODE:
		return "Invalid channel mode";
	case ERROR_CODE_A2DP_NOT_SUPPORTED_CHANNEL_MODE:
		return "Unsupported channel mode";
	case ERROR_CODE_A2DP_INVALID_SAMPLE_RATE:
		return "Invalid sample rate";
	case ERROR_CODE_A2DP_NOT_SUPPORTED_SAMPLE_RATE:
		return "Unsupported sample rate";
	case ERROR_CODE_A2DP_INVALID_SAMPLE_RATE_MUSIC:
		return "Invalid music sample rate";
	case ERROR_CODE_A2DP_NOT_SUPPORTED_SAMPLE_RATE_MUSIC:
		return "Unsupported music sample rate";
	case ERROR_CODE_A2DP_INVALID_SAMPLE_RATE_VOICE:
		return "Invalid voice sample rate";
	case ERROR_CODE_A2DP_NOT_SUPPORTED_SAMPLE_RATE_VOICE:
		return "Unsupported voice sample rate";
	case ERROR_CODE_A2DP_INVALID_BLOCK_LENGTH:
		return "Invalid block length";
	case ERROR_CODE_A2DP_NOT_SUPPORTED_BLOCK_LENGTH:
		return "Unsupported block length";
	case ERROR_CODE_A2DP_INVALID_SUB_BANDS:
		return "Invalid sub-bands";
	case ERROR_CODE_A2DP_NOT_SUPPORTED_SUB_BANDS:
		return "Unsupported sub-bands";
	case ERROR_CODE_A2DP_INVALID_ALLOCATION_METHOD:
		return "Invalid allocation method";
	case ERROR_CODE_A2DP_NOT_SUPPORTED_ALLOCATION_METHOD:
		return "Unsupported allocation method";
	case ERROR_CODE_A2DP_INVALID_MIN_BIT_POOL_VALUE:
		return "Invalid min bit-pool value";
	case ERROR_CODE_A2DP_NOT_SUPPORTED_MIN_BIT_POOL_VALUE:
		return "Unsupported min bit-pool value";
	case ERROR_CODE_A2DP_INVALID_MAX_BIT_POOL_VALUE:
		return "Invalid max bit-pool value";
	case ERROR_CODE_A2DP_NOT_SUPPORTED_MAX_BIT_POOL_VALUE:
		return "Unsupported max bit-pool value";
	case ERROR_CODE_A2DP_INVALID_LAYER:
		return "Invalid layer";
	case ERROR_CODE_A2DP_NOT_SUPPORTED_LAYER:
		return "Unsupported layer";
	case ERROR_CODE_A2DP_INVALID_OBJECT_TYPE:
		return "Invalid object type";
	case ERROR_CODE_A2DP_NOT_SUPPORTED_OBJECT_TYPE:
		return "Unsupported object type";
	case ERROR_CODE_A2DP_INVALID_DIRECTIONS:
		return "Invalid directions";
	case ERROR_CODE_A2DP_NOT_SUPPORTED_DIRECTIONS:
		return "Unsupported directions";
	case ERROR_CODE_A2DP_INVALID_FRAME_DURATION:
		return "Invalid frame duration";
	case ERROR_CODE_A2DP_NOT_SUPPORTED_FRAME_DURATION:
		return "Unsupported frame duration";
	case ERROR_CODE_A2DP_INVALID_BIT_DEPTH:
		return "Invalid bit depth";
	case ERROR_CODE_A2DP_NOT_SUPPORTED_BIT_DEPTH:
		return "Unsupported bit depth";
	}
	debug("Unknown error code: %#x", err);
	return "Unknown error";
}

/**
 * Get string representation of the error code.
 *
 * @param err Error code.
 * @return Human-readable string. */
const char * error_code_strerror(error_code_t err) {
	switch (ERROR_DOMAIN(err)) {
	case ERROR_DOMAIN_APP:
		return app_strerror(ERROR_CODE(err));
	case ERROR_DOMAIN_SYSTEM:
		return strerror(ERROR_CODE(err));
	}
	debug("Unknown error code: %#x", err);
	return "Unknown error";
}

#if ENABLE_MP3LAME
/**
 * Get string representation of LAME error code.
 *
 * @param err LAME encoder error code.
 * @return Human-readable string. */
const char * lame_encode_strerror(int err) {
	switch (err) {
	case -1:
		return "Too small output buffer";
	case -2:
		return "Out of memory";
	case -3:
		return "Params not initialized";
	case -4:
		return "Psycho acoustic error";
	}
	debug("Unknown error code: %#x", err);
	return "Unknown error";
}
#endif

#if ENABLE_AAC
/**
 * Get string representation of the FDK-AAC decoder error code.
 *
 * @param err FDK-AAC decoder error code.
 * @return Human-readable string. */
const char * aacdec_strerror(AAC_DECODER_ERROR err) {
	switch (err) {
	case AAC_DEC_OK:
		return "Success";
	case AAC_DEC_OUT_OF_MEMORY:
		return "Out of memory";
	case AAC_DEC_UNKNOWN:
		return "Unknown error";
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
	case AAC_DEC_OUTPUT_BUFFER_TOO_SMALL:
		return "Output buffer too small";
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
	case aac_dec_sync_error_start:
	case aac_dec_sync_error_end:
	case aac_dec_init_error_start:
	case aac_dec_init_error_end:
	case aac_dec_decode_error_start:
	case aac_dec_decode_error_end:
	case aac_dec_anc_data_error_start:
	case aac_dec_anc_data_error_end:
		break;
	}
	debug("Unknown error code: %#x", err);
	return "Unknown error";
}
#endif

#if ENABLE_AAC
/**
 * Get string representation of the FDK-AAC encoder error code.
 *
 * @param err FDK-AAC encoder error code.
 * @return Human-readable string. */
const char * aacenc_strerror(AACENC_ERROR err) {
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
	case AACENC_INIT_MPS_ERROR:
		return "MPS library initialization error";
	case AACENC_ENCODE_ERROR:
		return "Encoding error";
	case AACENC_ENCODE_EOF:
		return "End of file";
	}
	debug("Unknown error code: %#x", err);
	return "Unknown error";
}
#endif

#if ENABLE_LC3PLUS
/**
 * Get string representation of the LC3plus error code.
 *
 * @param err LC3plus error code.
 * @return Human-readable string. */
const char * lc3plus_strerror(LC3PLUS_Error err) {
	switch (err) {
	case LC3PLUS_OK:
		return "Success";
	case LC3PLUS_ERROR:
		return "Generic error";
	case LC3PLUS_DECODE_ERROR:
		return "Decode error";
	case LC3PLUS_NULL_ERROR:
		return "Invalid argument";
	case LC3PLUS_SAMPLERATE_ERROR:
		return "Invalid sample rate";
	case LC3PLUS_CHANNELS_ERROR:
		return "Invalid channel config";
	case LC3PLUS_BITRATE_ERROR:
		return "Invalid bitrate";
	case LC3PLUS_NUMBYTES_ERROR:
		return "Invalid number of bytes";
	case LC3PLUS_EPMODE_ERROR:
		return "Invalid EP mode";
	case LC3PLUS_FRAMEMS_ERROR:
		return "Invalid frame length";
	case LC3PLUS_ALIGN_ERROR:
		return "Unaligned pointer";
	case LC3PLUS_HRMODE_ERROR:
		return "Invalid hi-resolution mode";
	case LC3PLUS_BITRATE_UNSET_ERROR:
	case LC3PLUS_BITRATE_SET_ERROR:
		return "Bitrate set error";
	case LC3PLUS_HRMODE_BW_ERROR:
		return "Conflict hi-resolution mode and bandwidth switching";
	case LC3PLUS_PLCMODE_ERROR:
		return "Invalid PLC method";
	case LC3PLUS_EPMR_ERROR:
		return "Invalid EPMR value";
	case LC3PLUS_WARNING:
		return "Generic warning";
	case LC3PLUS_BW_WARNING:
		return "Invalid cutoff frequency";
	case LC3PLUS_PADDING_ERROR:
		return "Padding error";
	case LC3PLUS_LFE_MODE_NOT_SUPPORTED:
		return "LFE not supported";
	case FRAMESIZE_ERROR:
		return "Framesize error";
	}
	debug("Unknown error code: %#x", err);
	return "Unknown error";
}
#endif

#if ENABLE_LDAC
/**
 * Get string representation of the LDAC error code.
 *
 * @param err LDAC error code.
 * @return Human-readable string. */
const char * ldacBT_strerror(int err) {
	int code = LDACBT_HANDLE_ERR(err);
	switch (code != 0 ? code : LDACBT_API_ERR(err)) {
	case LDACBT_ERR_NONE:
		return "Success";
	case LDACBT_ERR_FATAL_HANDLE:
		return "Invalid handle";
	case LDACBT_ERR_HANDLE_NOT_INIT:
		return "Handle not initialized";
	case LDACBT_ERR_ENC_INIT_ALLOC:
	case LDACBT_ERR_DEC_INIT_ALLOC:
		return "Out of memory";
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
	case LDACBT_ERR_FRAME_LENGTH_OVER:
	case LDACBT_ERR_FRAME_ALIGN_OVER:
		return "Invalid frame";
	case LDACBT_ERR_ASSERT_NSHIFT:
		return "Invalid N-shift";
	case LDACBT_ERR_ASSERT_CHANNEL_MODE:
		return "Invalid channel mode";
	case LDACBT_ERR_ENC_ILL_GRADMODE:
	case LDACBT_ERR_ENC_ILL_GRADPAR_A:
	case LDACBT_ERR_ENC_ILL_GRADPAR_B:
	case LDACBT_ERR_ENC_ILL_GRADPAR_C:
	case LDACBT_ERR_ENC_ILL_GRADPAR_D:
		return "Invalid gradient parameter";
	case LDACBT_ERR_ENC_ILL_NBANDS:
		return "Invalid N-bands";
	case LDACBT_ERR_PACK_BLOCK_FAILED:
		return "Block packing error";
	case LDACBT_ERR_INPUT_BUFFER_SIZE:
		return "Too small input buffer";
	case LDACBT_ERR_UNPACK_BLOCK_FAILED:
	case LDACBT_ERR_UNPACK_BLOCK_ALIGN:
	case LDACBT_ERR_UNPACK_FRAME_ALIGN:
		return "Block unpacking error";
	case LDACBT_ERR_ILL_SYNCWORD:
		return "Invalid sync-word";
	case LDACBT_ERR_ILL_SMPL_FORMAT:
		return "Invalid sample format";
	case LDACBT_ERR_ILL_PARAM:
		return "Invalid parameter";
	case LDACBT_ERR_ILL_EQMID:
		return "Unsupported EQMID";
	case LDACBT_ERR_ILL_SAMPLING_FREQ:
		return "Unsupported sample rate";
	case LDACBT_ERR_ILL_NUM_CHANNEL:
		return "Unsupported channels";
	case LDACBT_ERR_ILL_MTU_SIZE:
		return "Unsupported MTU";
	case LDACBT_ERR_ALTER_EQMID_LIMITED:
		return "EQMID limited";
	case LDACBT_ERR_DEC_CONFIG_UPDATED:
		return "Configuration updated";
	}
	debug("Unknown error code: %#x (API: %u, handle: %u, block: %u)",
			err, LDACBT_API_ERR(err), LDACBT_HANDLE_ERR(err), LDACBT_BLOCK_ERR(err));
	return "Unknown error";
}
#endif

#if ENABLE_LHDC
/**
 * Get string representation of the LHDC decoder error code.
 *
 * @param err LHDC decoder error code.
 * @return Human-readable string. */
const char * lhdcBT_dec_strerror(int err) {
	switch (err) {
	case LHDCBT_DEC_FUNC_SUCCEED:
		return "Success";
	case LHDCBT_DEC_FUNC_FAIL:
		return "Decode failed";
	case LHDCBT_DEC_FUNC_INPUT_NOT_ENOUGH:
		return "Too small input buffer";
	case LHDCBT_DEC_FUNC_OUTPUT_NOT_ENOUGH:
		return "Output buffer too small";
	case LHDCBT_DEC_FUNC_INVALID_SEQ_NO:
		return "Invalid sequence number";
	}
	debug("Unknown error code: %#x", err);
	return "Unknown error";
}
#endif
