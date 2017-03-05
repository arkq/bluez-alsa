/*
 * BlueALSA - io-msbc.c
 * Copyright (c) 2017 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "io.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <sbc/sbc.h>
#include "hfp.h"
#include "bluealsa.h"
#include "transport.h"
#include "utils.h"
#include "io-msbc.h"
#include "shared/log.h"

#if ENABLE_MSBC

#define SCO_H2_HDR_0             0x01
#define MSBC_SYNC                0xAD
/* We seem to get the data in 24 byte chunks
 * even though the SCO MTU is 60
 * bytes. Use the same to send data
 * TODO: Figure out why.
 */
#define MSBC_MTU		24

#if defined (SILENCE)
uint8_t msbc_zero[] = {
	0xad, 0x0, 0x0, 0xc5, 0x0, 0x0, 0x0, 0x0, 0x77, 0x6d, 0xb6, 0xdd,
	0xdb, 0x6d, 0xb7, 0x76, 0xdb, 0x6d, 0xdd, 0xb6, 0xdb, 0x77, 0x6d,
	0xb6, 0xdd, 0xdb, 0x6d, 0xb7, 0x76, 0xdb, 0x6d, 0xdd, 0xb6, 0xdb,
	0x77, 0x6d, 0xb6, 0xdd, 0xdb, 0x6d, 0xb7, 0x76, 0xdb, 0x6d, 0xdd,
	0xb6, 0xdb, 0x77, 0x6d, 0xb6, 0xdd, 0xdb, 0x6d, 0xb7, 0x76, 0xdb,
	0x6c
};
#endif

int iothread_write_encoded_data(int bt_fd, struct sbc_state *sbc, size_t length)
{
	size_t written = 0;

	if (sbc->enc_buffer_cnt < length) {
		warn("Encoded data underflow");
		return -1;
	}

	if ((written = write(bt_fd, sbc->enc_buffer, length)) == -1) {
		if (errno != EWOULDBLOCK && errno != EAGAIN)
			warn("Could not write to mSBC socket: %s", strerror(errno));
		return -1;
	}

	memmove(sbc->enc_buffer,
		sbc->enc_buffer + written,
		sbc->enc_buffer_cnt - written);

	sbc->enc_buffer_cnt -= written;

	return 0;
}

static void iothread_encode_msbc_frames(struct sbc_state *sbc)
{
	static const uint8_t h2_header_frame_number[] = {
		0x08, 0x38, 0xc8, 0xf8
	};

	size_t written, pcm_consumed = 0;
	ssize_t len;

	/* Encode all we can */
	while ((sbc->enc_pcm_buffer_cnt - pcm_consumed) >= sbc->enc_pcm_size &&
	       (sbc->enc_buffer_size - sbc->enc_buffer_cnt) >= SCO_H2_FRAME_LEN) {

		struct msbc_frame *frame = (struct msbc_frame*)(sbc->enc_buffer
								+ sbc->enc_buffer_cnt);

		if ((len = sbc_encode(&sbc->enc,
				      sbc->enc_pcm_buffer + pcm_consumed,
				      sbc->enc_pcm_buffer_cnt - pcm_consumed,
				      frame->payload,
				      sizeof(frame->payload),
				      &written)) < 0) {
			error("Unable to encode mSBC: %s", strerror(-len));
			return;
		};

		pcm_consumed += len;

		frame->h2_header[0] = SCO_H2_HDR_0;
		frame->h2_header[1] = h2_header_frame_number[sbc->enc_frame_number];
		sbc->enc_frame_number = ((sbc->enc_frame_number) + 1) % 4;
		sbc->enc_buffer_cnt += sizeof(*frame);

#ifdef SILENCE
		memcpy(frame->payload, msbc_zero, sizeof(frame->payload));
#endif
	}
	/* Reshuffle remaining PCM samples to the beginning of the buffer */
	memmove(sbc->enc_pcm_buffer,
		sbc->enc_pcm_buffer + pcm_consumed,
		sbc->enc_pcm_buffer_cnt - pcm_consumed);

	/* And deduct consumed data */
	sbc->enc_pcm_buffer_cnt -= pcm_consumed;
}

static void iothread_find_and_decode_msbc(int pcm_fd, struct sbc_state *sbc)
{
	ssize_t len;
	size_t bytes_left = sbc->dec_buffer_cnt;
	uint8_t *p = (uint8_t*) sbc->dec_buffer;

	/* Find frame start */
	while (bytes_left >= (SCO_H2_HDR_LEN + sbc->sbc_frame_len)) {
		if (p[0] == SCO_H2_HDR_0 && p[2] == MSBC_SYNC) {
			/* Found frame.  TODO: Check SEQ, implement PLC */
			size_t decoded = 0;
			if ((len = sbc_decode(&sbc->dec,
					      p + 2,
					      sbc->sbc_frame_len,
					      sbc->dec_pcm_buffer,
					      sizeof(sbc->dec_pcm_buffer),
					      &decoded)) < 0) {
				error("mSBC decoding error: %s\n", strerror(-len));
				sbc->dec_buffer_cnt = 0;
				return;
			}
			bytes_left -= len + SCO_H2_HDR_LEN;
			p += len + SCO_H2_HDR_LEN;
			if (write(pcm_fd, sbc->dec_pcm_buffer, decoded) < 0)
				warn("Could not write PCM data: %s", strerror(errno));
		}
		else {
			bytes_left--;
			p++;
		}
	}
	memmove(sbc->dec_buffer, p, bytes_left);
	sbc->dec_buffer_cnt = bytes_left;
}

bool iothread_initialize_msbc(struct sbc_state *sbc)
{
	memset(sbc, 0, sizeof(*sbc));

	if (errno = -sbc_init_msbc(&sbc->dec, 0) != 0) {
		error("Couldn't initialize mSBC decoder: %s", strerror(errno));
		return false;
	}

	if (errno = -sbc_init_msbc(&sbc->enc, 0) != 0) {
		error("Couldn't initialize mSBC decoder: %s", strerror(errno));
		return false;
	}

	sbc->sbc_frame_len = sbc_get_frame_length(&sbc->dec);
	sbc->dec_buffer_size = sizeof(sbc->dec_buffer);

	sbc->enc_pcm_size = sbc_get_codesize(&sbc->enc);
	sbc->enc_frame_len = sbc_get_frame_length(&sbc->enc);
	sbc->enc_buffer_size = sizeof(sbc->enc_buffer);
	sbc->enc_pcm_buffer_size = sizeof(sbc->enc_pcm_buffer);
	if (sbc->enc_frame_len != MSBC_FRAME_LEN) {
		error("Unexpected mSBC frame size: %zd", sbc->enc_frame_len); 
	}

	return true;
}


int iothread_handle_incoming_msbc(struct ba_transport *t, struct sbc_state *sbc) {

	uint8_t *read_buf = sbc->dec_buffer + sbc->dec_buffer_cnt;
	size_t read_buf_size = sbc->dec_buffer_size - sbc->dec_buffer_cnt;
	ssize_t len;

	if ((len = read(t->bt_fd, read_buf, read_buf_size)) == -1) {
		debug("SCO read error: %s", strerror(errno));
		return -1;
	}

	sbc->dec_buffer_cnt += len;

	if (t->sco.mic_pcm.fd >= 0)
		iothread_find_and_decode_msbc(t->sco.mic_pcm.fd, sbc);
	else
		sbc->dec_buffer_cnt = 0; /* Drop microphone data if PCM isn't open */

	/* Synchronize write to read */
	if (t->sco.spk_pcm.fd >= 0) {
		iothread_write_encoded_data(t->bt_fd, sbc, MSBC_MTU);
		if ((sbc->enc_buffer_size - sbc->enc_buffer_cnt) >= SCO_H2_FRAME_LEN) {
			return 1;
		}
	}
	return 0;

}

int iothread_handle_outgoing_msbc(struct ba_transport *t, struct sbc_state *sbc) {

	ssize_t len;

	/* Read PCM samples */
	if ((len = read(t->sco.spk_pcm.fd,
			sbc->enc_pcm_buffer + sbc->enc_pcm_buffer_cnt,
			sbc->enc_pcm_buffer_size - sbc->enc_pcm_buffer_cnt)) == -1) {
		error("Unable to read PCM data: %s", strerror(errno));
		-1;
	}
	sbc->enc_pcm_buffer_cnt += len;

	/* Encode as much data as we can */
	iothread_encode_msbc_frames(sbc);

	return 1;
}

#endif
