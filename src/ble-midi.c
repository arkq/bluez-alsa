/*
 * BlueALSA - ble-midi.c
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "ble-midi.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/time.h>
#include <time.h>

#include "shared/log.h"
#include "shared/rt.h"

/**
 * Determine length of the MIDI message based on the status byte. */
static size_t ble_midi_message_len(uint8_t status) {
	switch (status & 0xF0) {
	case 0x80 /* note off */ :
	case 0x90 /* note on */ :
	case 0xA0 /* polyphonic key pressure */ :
	case 0xB0 /* control change */ :
		return 3;
	case 0xC0 /* program change */ :
	case 0xD0 /* channel pressure */ :
		return 2;
	case 0xE0 /* pitch bend */ :
		return 3;
	case 0xF0 /* system messages */ :
		switch (status) {
		case 0xF0 /* system exclusive start */ :
			/* System exclusive message size is unknown. It is just a
			 * stream of bytes terminated by the system exclusive end
			 * status byte. */
			return SIZE_MAX;
		case 0xF1 /* MIDI timing code */ :
			return 2;
		case 0xF2 /* song position pointer */ :
			return 3;
		case 0xF3 /* song select */ :
			return 2;
		case 0xF6 /* tune request */ :
		case 0xF7 /* system exclusive end */ :
		case 0xF8 /* timing clock */ :
		case 0xFA /* start sequence */ :
		case 0xFB /* continue sequence */ :
		case 0xFC /* stop sequence */ :
		case 0xFE /* active sensing */ :
		case 0xFF /* system reset */ :
			return 1;
		}
	}
	/* invalid status byte */
	return 0;
}

/**
 * Get SysEx buffer which can hold at least additional len bytes. */
static uint8_t *ble_midi_get_sys_buffer(struct ble_midi_dec *bmd, size_t len) {

	if (bmd->buffer_sys_len + len <= bmd->buffer_sys_size)
		goto final;

	uint8_t *tmp = bmd->buffer_sys;
	size_t size = bmd->buffer_sys_size + MAX(len, 512);
	if ((tmp = realloc(tmp, size * sizeof(*tmp))) == NULL) {
		warn("Couldn't resize BLE-MIDI SysEx buffer: %s", strerror(errno));
		goto final;
	}

	bmd->buffer_sys = tmp;
	bmd->buffer_sys_size = size;

final:
	return bmd->buffer_sys;
}

/**
 * Initialize BLE-MIDI decoder. */
void ble_midi_decode_init(struct ble_midi_dec *bmd) {
	memset(bmd, 0, sizeof(*bmd));
	gettimestamp(&bmd->ts0);
}

/**
 * Free BLE-MIDI decoder resources. */
void ble_midi_decode_free(struct ble_midi_dec *bmd) {
	free(bmd->buffer_sys);
}

/**
 * Decode BLE-MIDI packet.
 *
 * Before decoding next BLE-MIDI packet, this function should be called until
 * it returns 0 or -1. Alternatively, caller can set the decoder structure to
 * all-zeroes, which will reset the decoding state.
 *
 * @param bmd BLE-MIDI decoder structure.
 * @param data BLE-MIDI packet data.
 * @param len Length of the packet data.
 * @return On success, in case when at least one full MIDI message was decoded,
 *   this function returns 1. If the BLE-MIDI packet does not contain any more
 *   (complete) MIDI message, 0 is returned. On error, -1 is returned. */
int ble_midi_decode(struct ble_midi_dec *bmd, const uint8_t *data, size_t len) {

	uint8_t *bm_buffer = bmd->buffer_midi;
	size_t bm_buffer_size = sizeof(bmd->buffer_midi);
	size_t bm_buffer_len = 0;

	uint8_t bm_status = bmd->status;
	size_t bm_current_len = bmd->current_len;

	/* Check if we've got any data to parse. */
	if (bm_current_len == len)
		goto reset;

	/* If the system exclusive message was not ended in the previous
	 * packet we need to reconstruct fragmented message. */
	if (bmd->status_sys) {
		bm_buffer = ble_midi_get_sys_buffer(bmd, len);
		bm_buffer_size = bmd->buffer_sys_size;
		bm_buffer_len = bmd->buffer_sys_len;
		bm_status = 0xF0;
	}

	/* Every BLE-MIDI packet shall contain a header byte. */
	if (bm_current_len == 0) {
		/* There should be at least 3 bytes in the packet: header,
		 * timestamp and at least one MIDI message byte. */
		if (len < 3 || (data[0] >> 6) != 0x02) {
			errno = EINVAL;
			goto fail;
		}
		/* Extract most significant 6 bits of the 13-bits timestamp
		 * from the header. */
		bmd->ts_high = data[0] & 0x3F;
		bmd->ts_low = 0;
		bm_current_len++;
	}

retry:
	/* Check if we've got BLE-MIDI timestamp byte.
	 * It shall have bit 7 set to 1. */
	if (data[bm_current_len] & 0x80) {

		/* Increment timestamp-high in case of timestamp-low
		 * overflow in the current BLE-MIDI packet. */
		unsigned int ts_low = data[bm_current_len] & 0x7F;
		if (bm_current_len > 1 && ts_low < bmd->ts_low)
			bmd->ts_high += 1;

		unsigned int ts_high_low = (bmd->ts_high << 7) | ts_low;

		/* The timestamp in the BLE-MIDI packet shall increase monotonically, so
		 * it is possible to calculate the timestamp difference between packets.
		 * However, the absolute value of the timestamp is in the remote device
		 * clock domain. We need a mechanism to synchronize the monotonic time
		 * with our local clock. */

		int ts_high_low_diff = ts_high_low - bmd->ts_high_low;
		if (ts_high_low_diff < 0)
			ts_high_low_diff += 8191;

		struct timespec ts = {
			.tv_sec = ts_high_low_diff / 1000,
			.tv_nsec = (ts_high_low_diff % 1000) * 1000000 };
		timespecadd(&bmd->ts, &ts, &bmd->ts);

		/* Check timestamp drift based on the first timestamp byte in the
		 * BLE-MIDI packet. The packet may contain many MIDI messages which
		 * can span in time. However, in order to allow real-time playback,
		 * the first MIDI message shall be played as soon as possible. */
		if (bm_current_len == 1) {

			static const struct timespec ts_1ms = { .tv_nsec = 1000000 };
			static const struct timespec ts_5ms = { .tv_nsec = 5000000 };

			struct timespec now;
			struct timespec ts0_time;

			gettimestamp(&now);
			timespecsub(&now, &bmd->ts0, &ts0_time);

			/* Calculate the time difference between the BLE-MIDI
			 * session duration based on host clock and based on the
			 * BLE-MIDI packets timestamps. */
			struct timespec diff;
			int rv = difftimespec(&ts0_time, &bmd->ts, &diff);
			time_t diff_ms = timespec2ms(&diff);

			if (diff_ms > 500) {
				debug("BLE-MIDI time synchronization reset");
				bmd->ts = ts0_time;
			}
			else if (rv < 0) {
				if (diff_ms > 100)
					timespecadd(&bmd->ts, &ts_5ms, &bmd->ts);
				else if (diff_ms > 15)
					timespecadd(&bmd->ts, &ts_1ms, &bmd->ts);
			}
			else if (rv > 0) {
				if (diff_ms > 100)
					timespecsub(&bmd->ts, &ts_5ms, &bmd->ts);
				else if (diff_ms > 15)
					timespecsub(&bmd->ts, &ts_1ms, &bmd->ts);
			}

		}

		bmd->ts_low = ts_low;
		bmd->ts_high_low = ts_high_low;

		if (++bm_current_len == len) {
			/* If the timestamp byte is the last byte in the packet,
			 * definitely something is wrong. */
			errno = EINVAL;
			goto fail;
		}

		/* After the timestamp byte, there might be a full MIDI message
		 * with a status byte. It shall have bit 7 set to 1. Otherwise,
		 * it might be a running status MIDI message. */
		if (data[bm_current_len] & 0x80) {

			switch (bm_status = data[bm_current_len]) {
			case 0xF0 /* system exclusive start */ :
				/* System exclusive message needs to be stored in a dedicated buffer.
				 * First of all, it can span multiple BLE-MIDI packets. Secondly, it
				 * can be interleaved with MIDI real-time messages. */
				bm_buffer = ble_midi_get_sys_buffer(bmd, len);
				bm_buffer_size = bmd->buffer_sys_size;
				bm_buffer_len = bmd->buffer_sys_len;
				bmd->status_sys = true;
				break;
			case 0xF7 /* system exclusive end */ :
				bmd->status_sys = false;
				break;
			}

			/* Store full MIDI message status byte in the buffer. */
			if (bm_buffer_len < bm_buffer_size)
				bm_buffer[bm_buffer_len++] = bm_status;

			if (++bm_current_len == len)
				goto final;

		}

	}

	/* Fix for BLE-MIDI vs MIDI incompatible running status. */
	if (bm_buffer_len == 0 && bmd->status_restore) {
		bm_buffer[bm_buffer_len++] = bm_status;
		bmd->status_restore = false;
	}

	size_t midi_msg_len;
	if ((midi_msg_len = ble_midi_message_len(bm_status)) == 0) {
		errno = ENOMSG;
		goto fail;
	}

	/* Extract MIDI message data bytes. All these bytes shall have
	 * bit 7 set to 0. */
	while (--midi_msg_len > 0 && !(data[bm_current_len] & 0x80) &&
			/* Make sure that we do not overflow the buffer. */
			bm_buffer_len < bm_buffer_size) {
		bm_buffer[bm_buffer_len++] = data[bm_current_len];
		if (++bm_current_len == len)
			goto final;
	}

	/* MIDI message cannot be incomplete. */
	if (midi_msg_len != 0 && bm_status != 0xF0) {
		errno = EBADMSG;
		goto fail;
	}

	if (bm_buffer_len == bm_buffer_size) {
		warn("BLE-MIDI message too long: %zu", bm_buffer_size);
		errno = EMSGSIZE;
		goto final;
	}

	/* This parser reads only one MIDI message at a time. However, in case of
	 * the system exclusive message, instead of returning not-completed MIDI
	 * message, check if the message was ended in this BLE-MIDI packet. */
	if (bm_status == 0xF0) {
		bmd->buffer_sys_len = bm_buffer_len;
		goto retry;
	}

final:

	bmd->buffer = bm_buffer;
	bmd->len = bm_buffer_len;

	/* In BLE-MIDI, MIDI real-time messages and MIDI common messages do not
	 * affect the running status. For simplicity, we will not store running
	 * status for every system message. */
	if ((bm_status & 0xF0) != 0xF0)
		bmd->status = bm_status;

	/* According to the BLE-MIDI specification, the running status is not
	 * cancelled by the system common messages. However, for MIDI, running
	 * status is not cancelled by the system real-time messages only. So,
	 * for everything other than the system real-time messages, we need to
	 * insert the status byte into the buffer. */
	if (bm_status >= 0xF0 && bm_status < 0xF8)
		bmd->status_restore = true;

	bmd->current_len = bm_current_len;

	switch (bm_status) {
	case 0xF0 /* system exclusive start */ :
		bmd->buffer_sys_len = bm_buffer_len;
		goto reset;
	case 0xF7 /* system exclusive end */ :
		bmd->buffer_sys_len = 0;
		break;
	}

	return 1;

reset:
	bmd->current_len = 0;
	return 0;

fail:
	bmd->current_len = 0;
	return -1;
}

/**
 * Initialize BLE-MIDI encoder. */
void ble_midi_encode_init(struct ble_midi_enc *bme) {
	memset(bme, 0, sizeof(*bme));
}

/**
 * Encode BLE-MIDI packet.
 *
 * It is possible that a single MIDI system exclusive message will not fit
 * into the MTU of the BLE link. In such case, this function will return 1
 * and the caller should call this function again with the same MIDI message.
 * The encoder structure should not be modified between consecutive calls.
 *
 * @param bme BLE-MIDI encoder structure.
 * @param data Single MIDI message.
 * @param len Length of the MIDI message data.
 * @return On success, this function returns 0. If the BLE-MIDI packet has to
 *   be split into multiple packets, 1 is returned. On error, -1 is returned. */
int ble_midi_encode(struct ble_midi_enc *enc, const uint8_t *data, size_t len) {

	const bool is_sys = data[0] == 0xF0;
	bool is_sys_continue = false;
	size_t transfer_len = len;

	/* Check if the MTU is at least 5 bytes (header + timestamp + MIDI message)
	 * and does not exceed the buffer size of the encoder structure. */
	if (enc->mtu < 5 || enc->mtu > sizeof(enc->buffer)) {
		error("Invalid BLE-MIDI encoder MTU: %zu", enc->mtu);
		return errno = EINVAL, -1;
	}

	/* Check if the message will fit within the MTU. In case of consecutive
	 * encode calls, this check is off by one, but we can live with that. This
	 * check does not apply to the system exclusive messages. */
	if (enc->len + 2 + len > enc->mtu && !is_sys)
		return errno = EMSGSIZE, -1;

	/* Check if the message is a system exclusive message
	 * and if it is a continuation call. */
	if (is_sys && enc->len == enc->mtu) {
		is_sys_continue = true;
		enc->len = 0;
	}

	struct timespec now;
	gettimestamp(&now);
	unsigned int ts_high_low = now.tv_sec * 1000 + now.tv_nsec / 1000000;

	if (enc->len == 0) {
		/* Construct the BLE-MIDI header with the most significant
		* 6 bits of the 13-bits milliseconds timestamp. */
		enc->buffer[enc->len++] = 0x80 | ((ts_high_low >> 7) & 0x3F);
	}

	if (!is_sys_continue) {
		/* Add the timestamp byte with the least significant 7 bits
		 * of the timestamp. */
		enc->buffer[enc->len++] = 0x80 | (ts_high_low & 0x7F);
	}

	if (is_sys)
		/* Calculate the number of bytes that we can transfer. */
		transfer_len = MIN(len - enc->current_len, enc->mtu - enc->len);

	memcpy(&enc->buffer[enc->len], &data[enc->current_len], transfer_len);
	enc->len += transfer_len;

	if (is_sys) {
		if ((enc->current_len += transfer_len) != len)
			return 1;
		enc->current_len = 0;
	}

	return 0;
}

/**
 * Set BLE-MIDI encoder MTU. */
int ble_midi_encode_set_mtu(struct ble_midi_enc *bme, size_t mtu) {
	bme->mtu = MIN(mtu, sizeof(bme->buffer));
	return 0;
}
