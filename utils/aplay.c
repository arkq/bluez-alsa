/*
 * BlueALSA - aplay.c
 * Copyright (c) 2016-2017 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <alsa/asoundlib.h>

#include "shared/ctl-client.h"
#include "shared/log.h"


bool main_loop_on = true;
static void main_loop_stop(int sig) {
	/* Call to this handler restores the default action, so on the
	 * second call the program will be forcefully terminated. */

	struct sigaction sigact = { .sa_handler = SIG_DFL };
	sigaction(sig, &sigact, NULL);

	main_loop_on = false;
}

static int set_hw_params(snd_pcm_t *pcm, int channels, int rate) {

	snd_pcm_hw_params_t *params;
	unsigned int size;
	int dir;
	int err;

	snd_pcm_hw_params_alloca(&params);

	if ((err = snd_pcm_hw_params_any(pcm, params)) != 0)
		return err;
	if ((err = snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED)) != 0)
		return err;
	if ((err = snd_pcm_hw_params_set_format(pcm, params, SND_PCM_FORMAT_S16_LE)) != 0)
		return err;
	if ((err = snd_pcm_hw_params_set_channels(pcm, params, channels)) != 0)
		return err;
	if ((err = snd_pcm_hw_params_set_rate(pcm, params, rate, 0)) != 0)
		return err;
	size = 500000;
	if ((err = snd_pcm_hw_params_set_buffer_time_near(pcm, params, &size, &dir)) != 0)
		return err;
	size = 100000;
	if ((err = snd_pcm_hw_params_set_period_time_near(pcm, params, &size, &dir)) != 0)
		return err;
	if ((err = snd_pcm_hw_params(pcm, params)) != 0)
		return err;

	return 0;
}

static int set_sw_params(snd_pcm_t *pcm, snd_pcm_uframes_t buffer_size, snd_pcm_uframes_t period_size) {

	snd_pcm_sw_params_t *params;
	int err;

	snd_pcm_sw_params_alloca(&params);

	if ((err = snd_pcm_sw_params_current(pcm, params)) != 0)
		return err;
	/* start the transfer when the buffer is almost full */
	snd_pcm_uframes_t threshold = (buffer_size / period_size) * period_size;
	if ((err = snd_pcm_sw_params_set_start_threshold(pcm, params, threshold)) != 0)
		return err;
	/* allow the transfer when at least period_size samples can be processed */
	if ((err = snd_pcm_sw_params_set_avail_min(pcm, params, period_size)) != 0)
		return err;
	if ((err = snd_pcm_sw_params(pcm, params)) != 0)
		return err;

	return 0;
}

int main(int argc, char *argv[]) {

	int opt;
	const char *opts = "hi:d:";
	const struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "hci", required_argument, NULL, 'i' },
		{ "pcm", required_argument, NULL, 'd' },
		{ "profile-a2dp", no_argument, NULL, 1 },
		{ "profile-sco", no_argument, NULL, 2 },
		{ 0, 0, 0, 0 },
	};

	const char *device = "default";
	const char *ba_interface = "hci0";
	enum pcm_type ba_type = PCM_TYPE_A2DP;

	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1)
		switch (opt) {
		case 'h' /* --help */ :
usage:
			printf("usage: %s [OPTION]... <BT-ADDR>\n\n"
					"  -h, --help\t\tprint this help and exit\n"
					"  -i, --hci=hciX\tHCI device to use\n"
					"  -d, --pcm=NAME\tPCM device to use\n"
					"  --profile-a2dp\tuse A2DP profile\n"
					"  --profile-sco\t\tuse SCO profile\n",
					argv[0]);
			return EXIT_SUCCESS;

		case 'i' /* --hci */ :
			ba_interface = optarg;
			break;
		case 'd' /* --pcm */ :
			device = optarg;
			break;

		case 1 /* --profile-a2dp */ :
			ba_type = PCM_TYPE_A2DP;
			break;
		case 2 /* --profile-sco */ :
			ba_type = PCM_TYPE_SCO;
			break;

		default:
			fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
			return EXIT_FAILURE;
		}

	if (optind + 1 != argc)
		goto usage;

	struct msg_transport *transport = NULL;
	snd_pcm_t *pcm = NULL;
	int status = EXIT_SUCCESS;
	int ba_pcm_fd = -1;
	int ba_fd = -1;
	bdaddr_t addr;
	int err;

	log_open(argv[0], false, false);

	if (str2ba(argv[optind], &addr) != 0) {
		error("Invalid BT device address: %s", argv[optind]);
		goto fail;
	}

	if ((err = snd_pcm_open(&pcm, device, SND_PCM_STREAM_PLAYBACK, 0)) != 0) {
		error("Couldn't open PCM: %s", snd_strerror(err));
		goto fail;
	}

	if ((ba_fd = bluealsa_open(ba_interface)) == -1) {
		error("BlueALSA connection failed: %s", strerror(errno));
		goto fail;
	}

	if ((transport = bluealsa_get_transport(ba_fd, addr, ba_type, PCM_STREAM_CAPTURE)) == NULL) {
		error("Couldn't get BlueALSA transport: %s", strerror(errno));
		goto fail;
	}

	if ((err = set_hw_params(pcm, transport->channels, transport->sampling)) != 0) {
		error("Couldn't set HW parameters: %s", snd_strerror(err));
		goto fail;
	}

	snd_pcm_uframes_t buffer_size, period_size;
	if ((err = snd_pcm_get_params(pcm, &buffer_size, &period_size)) != 0) {
		error("Couldn't get PCM parameters: %s", snd_strerror(err));
		goto fail;
	}

	if ((err = set_sw_params(pcm, buffer_size, period_size)) != 0) {
		error("Couldn't set SW parameters: %s", snd_strerror(err));
		goto fail;
	}

	if ((err = snd_pcm_prepare(pcm)) != 0) {
		error("Couldn't prepare PCM: %s", snd_strerror(err));
		goto fail;
	}

	transport->stream = PCM_STREAM_CAPTURE;
	if ((ba_pcm_fd = bluealsa_open_transport(ba_fd, transport)) == -1) {
		error("Couldn't open PCM FIFO: %s", strerror(errno));
		goto fail;
	}

	struct sigaction sigact = { .sa_handler = main_loop_stop };
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGINT, &sigact, NULL);

	struct pollfd pfds[] = {{ ba_pcm_fd, POLLIN, 0 }};
	int frame_size = transport->channels * 2;
	char *buffer = malloc(period_size * frame_size);
	char *buffer_head = buffer;

	debug("Starting main loop");
	while (main_loop_on) {

		size_t buffer_len = period_size * frame_size - (buffer_head - buffer);
		ssize_t ret;

		/* Reading from the FIFO won't block unless there is an open connection
		 * on the writing side. However, the server does not open PCM FIFO until
		 * a transport is created. With the A2DP, the transport is created when
		 * some clients (BT device) requests audio transfer. */
		if (poll(pfds, sizeof(pfds) / sizeof(*pfds), -1) == -1 && errno == EINTR)
			continue;

		if ((ret = read(ba_pcm_fd, buffer_head, buffer_len)) == -1) {
			if (errno == EINTR)
				continue;
			error("PCM FIFO read error: %s", strerror(errno));
			goto fail;
		}

		/* calculate the overall number of frames in the buffer */
		snd_pcm_uframes_t frames = ((buffer_head - buffer) + ret) / frame_size;

		if ((err = snd_pcm_writei(pcm, buffer, frames)) < 0)
			switch (-err) {
			case EPIPE:
				debug("An underrun has occurred");
				snd_pcm_prepare(pcm);
				usleep(50000);
				err = 0;
				break;
			default:
				error("Couldn't write to PCM: %s", snd_strerror(err));
				goto fail;
			}

		size_t writei_len = err * frame_size;
		buffer_head = buffer;

		/* move leftovers to the beginning and reposition head */
		if ((size_t)ret > writei_len) {
			memmove(buffer, buffer + writei_len, ret - writei_len);
			buffer_head += ret - writei_len;
		}

	}

	goto success;

fail:
	status = EXIT_FAILURE;

success:
	if (ba_pcm_fd != -1) {
		bluealsa_close_transport(ba_fd, transport);
		close(ba_pcm_fd);
	}
	free(transport);
	if (ba_fd != -1)
		close(ba_fd);
	if (pcm != NULL)
		snd_pcm_close(pcm);
	return status;
}
