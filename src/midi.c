/*
 * BlueALSA - midi.c
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "midi.h"

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <alsa/asoundlib.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <glib.h>

#include "ba-adapter.h"
#include "ba-device.h"
#include "ba-transport.h"
#include "ble-midi.h"
#include "utils.h"
#include "shared/log.h"

static gboolean midi_watch_read_alsa_seq(G_GNUC_UNUSED GIOChannel *ch,
		G_GNUC_UNUSED GIOCondition condition, void *userdata) {

	struct ba_transport *t = userdata;
	unsigned char buf[1024];
	long len;
	int rv;

	if (t->midi.ble_fd_notify == -1) {
		/* Drop all events if notification is not acquired. */
		snd_seq_drop_input(t->midi.seq);
		return TRUE;
	}

	snd_seq_event_t *ev;
	while (snd_seq_event_input(t->midi.seq, &ev) >= 0) {

		if ((len = snd_midi_event_decode(t->midi.seq_parser, buf, sizeof(buf), ev)) < 0)
			error("Couldn't decode MIDI event: %s", snd_strerror(len));
		else {

retry:
			rv = ble_midi_encode(&t->midi.ble_encoder, buf, len);
			if (rv == 1 || (rv == -1 && errno == EMSGSIZE)) {
				/* Write out encoded BLE-MIDI packet to the socket. */
				if (write(t->midi.ble_fd_notify, t->midi.ble_encoder.buffer,
							t->midi.ble_encoder.len) != (ssize_t)t->midi.ble_encoder.len)
					error("BLE-MIDI link write error: %s", strerror(errno));
				if (rv == -1) {
					/* Reset encoder state and try again. */
					t->midi.ble_encoder.len = 0;
					goto retry;
				}
			}
			else if (rv == -1)
				error("Couldn't encode MIDI event: %s", strerror(errno));

		}

	}

	/* Write out encoded BLE-MIDI packet to the socket. */
	if (write(t->midi.ble_fd_notify, t->midi.ble_encoder.buffer,
				t->midi.ble_encoder.len) != (ssize_t)t->midi.ble_encoder.len)
		error("BLE-MIDI link write error: %s", strerror(errno));
	t->midi.ble_encoder.len = 0;

	return TRUE;
}

static gboolean midi_watch_read_ble_midi(GIOChannel *ch,
		G_GNUC_UNUSED GIOCondition condition, void *userdata) {

	struct ba_transport *t = userdata;
	GError *err = NULL;
	uint8_t data[512];
	long encoded;
	size_t len;
	int rv;

	switch (g_io_channel_read_chars(ch, (char *)data, sizeof(data), &len, &err)) {
	case G_IO_STATUS_AGAIN:
		return TRUE;
	case G_IO_STATUS_ERROR:
		error("BLE-MIDI link read error: %s", err->message);
		g_error_free(err);
		return TRUE;
	case G_IO_STATUS_NORMAL:
		break;
	case G_IO_STATUS_EOF:
		/* remove channel from watch */
		return FALSE;
	}

	snd_seq_event_t ev = { 0 };
	snd_seq_ev_set_source(&ev, t->midi.seq_port);
	snd_seq_ev_set_subs(&ev);

	for (;;) {

		if ((rv = ble_midi_decode(&t->midi.ble_decoder, data, len)) <= 0) {
			if (rv == -1) {
				error("Couldn't parse BLE-MIDI packet: %s", strerror(errno));
				hexdump("BLE-MIDI packet", data, len);
			}
			break;
		}

		if ((encoded = snd_midi_event_encode(t->midi.seq_parser,
						t->midi.ble_decoder.buffer, t->midi.ble_decoder.len, &ev)) < 0) {
				error("Couldn't encode MIDI event: %s", snd_strerror(encoded));
				continue;
		}

		snd_seq_real_time_t rt = {
			.tv_sec = t->midi.ble_decoder.ts.tv_sec,
			.tv_nsec = t->midi.ble_decoder.ts.tv_nsec };
		snd_seq_ev_schedule_real(&ev, t->midi.seq_queue, 0, &rt);

		if ((rv = snd_seq_event_output(t->midi.seq, &ev)) < 0)
			error("Couldn't send MIDI event: %s", snd_strerror(rv));

	}

	if ((rv = snd_seq_drain_output(t->midi.seq)) < 0)
		warn("Couldn't drain MIDI output: %s", snd_strerror(rv));

	return TRUE;
}

int midi_transport_alsa_seq_create(struct ba_transport *t) {

	const struct ba_device *d = t->d;
	snd_seq_client_info_t *info;
	snd_seq_t *seq = NULL;
	int port, queue;
	int rv;

	if ((rv = snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK)) != 0) {
		error("Couldn't open ALSA sequencer: %s", snd_strerror(rv));
		goto fail;
	}

	snd_seq_client_info_alloca(&info);
	if ((rv = snd_seq_get_client_info(seq, info)) != 0) {
		error("Couldn't get ALSA sequencer client info: %s", snd_strerror(rv));
		goto fail;
	}

	snd_seq_client_info_event_filter_add(info, SND_SEQ_EVENT_NOTEON);
	snd_seq_client_info_event_filter_add(info, SND_SEQ_EVENT_NOTEOFF);
	snd_seq_client_info_event_filter_add(info, SND_SEQ_EVENT_KEYPRESS);

	snd_seq_client_info_event_filter_add(info, SND_SEQ_EVENT_CONTROLLER);
	snd_seq_client_info_event_filter_add(info, SND_SEQ_EVENT_PGMCHANGE);
	snd_seq_client_info_event_filter_add(info, SND_SEQ_EVENT_CHANPRESS);
	snd_seq_client_info_event_filter_add(info, SND_SEQ_EVENT_PITCHBEND);
	snd_seq_client_info_event_filter_add(info, SND_SEQ_EVENT_CONTROL14);
	snd_seq_client_info_event_filter_add(info, SND_SEQ_EVENT_NONREGPARAM);
	snd_seq_client_info_event_filter_add(info, SND_SEQ_EVENT_REGPARAM);

	snd_seq_client_info_event_filter_add(info, SND_SEQ_EVENT_SONGPOS);
	snd_seq_client_info_event_filter_add(info, SND_SEQ_EVENT_SONGSEL);
	snd_seq_client_info_event_filter_add(info, SND_SEQ_EVENT_QFRAME);
	snd_seq_client_info_event_filter_add(info, SND_SEQ_EVENT_TIMESIGN);
	snd_seq_client_info_event_filter_add(info, SND_SEQ_EVENT_KEYSIGN);

	snd_seq_client_info_event_filter_add(info, SND_SEQ_EVENT_START);
	snd_seq_client_info_event_filter_add(info, SND_SEQ_EVENT_CONTINUE);
	snd_seq_client_info_event_filter_add(info, SND_SEQ_EVENT_STOP);
	snd_seq_client_info_event_filter_add(info, SND_SEQ_EVENT_CLOCK);

	snd_seq_client_info_event_filter_add(info, SND_SEQ_EVENT_TUNE_REQUEST);
	snd_seq_client_info_event_filter_add(info, SND_SEQ_EVENT_RESET);
	snd_seq_client_info_event_filter_add(info, SND_SEQ_EVENT_SENSING);

	snd_seq_client_info_event_filter_add(info, SND_SEQ_EVENT_SYSEX);

	snd_seq_client_info_set_name(info, "BlueALSA");

	if ((rv = snd_seq_set_client_info(seq, info)) != 0) {
		error("Couldn't set ALSA sequencer client info: %s", snd_strerror(rv));
		goto fail;
	}

	char addrstr[18];
	char name[10 + sizeof(addrstr)] = "BLE MIDI Server";
	if (bacmp(&d->addr, &d->a->hci.bdaddr) != 0) {
		ba2str(&d->addr, addrstr);
		snprintf(name, sizeof(name), "BLE MIDI %s", addrstr);
	}

	if ((port = rv = snd_seq_create_simple_port(seq, name,
				SND_SEQ_PORT_CAP_DUPLEX | SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_WRITE |
				SND_SEQ_PORT_CAP_SUBS_READ | SND_SEQ_PORT_CAP_SUBS_WRITE,
				SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_SOFTWARE)) < 0) {
		error("Couldn't create MIDI port: %s", snd_strerror(rv));
		goto fail;
	}

	if ((queue = rv = snd_seq_alloc_queue(seq)) < 0) {
		error("Couldn't allocate ALSA sequencer queue: %s", snd_strerror(rv));
		goto fail;
	}

	debug("Created new ALSA sequencer port: %d:%d",
			snd_seq_client_id(seq), rv);

	t->midi.seq = seq;
	t->midi.seq_port = port;
	t->midi.seq_queue = queue;

	return 0;

fail:
	if (seq != NULL)
		snd_seq_close(seq);
	return -1;
}

int midi_transport_alsa_seq_delete(struct ba_transport *t) {

	if (t->midi.seq == NULL)
		return 0;

	debug("Releasing ALSA sequencer port: %d:%d",
			snd_seq_client_id(t->midi.seq), t->midi.seq_port);

	snd_seq_free_queue(t->midi.seq, t->midi.seq_queue);
	t->midi.seq_queue = -1;
	snd_seq_delete_simple_port(t->midi.seq, t->midi.seq_port);
	t->midi.seq_port = -1;
	snd_seq_close(t->midi.seq);
	t->midi.seq = NULL;

	return 0;
}

int midi_transport_start_watch_alsa_seq(struct ba_transport *t) {

	struct pollfd pfd;
	snd_seq_poll_descriptors(t->midi.seq, &pfd, 1, POLLIN);

	debug("Starting ALSA sequencer IO watch: %d", pfd.fd);

	GIOChannel *ch = g_io_channel_unix_new(pfd.fd);
	g_io_channel_set_encoding(ch, NULL, NULL);
	g_io_channel_set_buffered(ch, FALSE);
	t->midi.watch_seq = g_io_create_watch_full(ch, G_PRIORITY_HIGH,
			G_IO_IN, midi_watch_read_alsa_seq, ba_transport_ref(t),
			(GDestroyNotify)ba_transport_unref);
	g_io_channel_unref(ch);

	ble_midi_encode_init(&t->midi.ble_encoder);

	return 0;
}

int midi_transport_start_watch_ble_midi(struct ba_transport *t) {

	debug("Starting BLE-MIDI IO watch: %d", t->midi.ble_fd_write);

	GIOChannel *ch = g_io_channel_unix_new(t->midi.ble_fd_write);
	g_io_channel_set_close_on_unref(ch, TRUE);
	g_io_channel_set_encoding(ch, NULL, NULL);
	g_io_channel_set_buffered(ch, FALSE);
	t->midi.watch_ble = g_io_create_watch_full(ch, G_PRIORITY_HIGH,
			G_IO_IN, midi_watch_read_ble_midi, ba_transport_ref(t),
			(GDestroyNotify)ba_transport_unref);
	g_io_channel_unref(ch);

	ble_midi_decode_init(&t->midi.ble_decoder);
	snd_seq_start_queue(t->midi.seq, t->midi.seq_queue, NULL);
	snd_seq_drain_output(t->midi.seq);

	return 0;
}

int midi_transport_start(struct ba_transport *t) {
	snd_midi_event_init(t->midi.seq_parser);
	midi_transport_start_watch_alsa_seq(t);
	return 0;
}

int midi_transport_stop(struct ba_transport *t) {

	if (t->midi.watch_seq != NULL) {
		g_source_destroy(t->midi.watch_seq);
		g_source_unref(t->midi.watch_seq);
		t->midi.watch_seq = NULL;
	}
	if (t->midi.watch_ble != NULL) {
		snd_seq_stop_queue(t->midi.seq, t->midi.seq_queue, NULL);
		g_source_destroy(t->midi.watch_ble);
		g_source_unref(t->midi.watch_ble);
		t->midi.watch_ble = NULL;
	}

	return 0;
}
