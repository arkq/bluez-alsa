/*
 * sndalign.c
 * Copyright (c) 2016-2024 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/param.h>
#include <sys/types.h>

#include <sndfile.h>

static bool is_silence(const short *src, unsigned int channels, size_t frames) {
	for (size_t i = 0; i < channels * frames; i++)
		if (src[i] != 0)
			return false;
	return true;
}

int main(int argc, char *argv[]) {

	if (argc != 3) {
		printf("Usage: %s <file1> <file2>\n", argv[0]);
		return EXIT_FAILURE;
	}

	SNDFILE *sf1;
	SF_INFO sf1_info = { 0 };
	if ((sf1 = sf_open(argv[1], SFM_READ, &sf1_info)) == NULL) {
		fprintf(stderr, "ERR: Couldn't open audio file: %s: %s\n", argv[1], sf_strerror(NULL));
		return EXIT_FAILURE;
	}

	printf("Source 1: %s\n", argv[1]);
	printf("  Frames: %ld\n", sf1_info.frames);
	printf("  Rate: %d\n", sf1_info.samplerate);
	printf("  Channels: %d\n", sf1_info.channels);

	SNDFILE *sf2;
	SF_INFO sf2_info = { 0 };
	if ((sf2 = sf_open(argv[2], SFM_READ, &sf2_info)) == NULL) {
		fprintf(stderr, "ERR: Couldn't open audio file: %s: %s\n", argv[2], sf_strerror(NULL));
		return EXIT_FAILURE;
	}

	printf("Source 2: %s\n", argv[2]);
	printf("  Frames: %ld\n", sf2_info.frames);
	printf("  Rate: %d\n", sf2_info.samplerate);
	printf("  Channels: %d\n", sf2_info.channels);

	if (sf1_info.channels != sf2_info.channels) {
		fprintf(stderr, "ERR: Channels mismatch: %d != %d\n", sf1_info.channels, sf2_info.channels);
		return EXIT_FAILURE;
	}

	if (sf1_info.samplerate != sf2_info.samplerate) {
		fprintf(stderr, "ERR: Sample rate mismatch: %d != %d\n", sf1_info.samplerate, sf2_info.samplerate);
		return EXIT_FAILURE;
	}

	short *sf1_data = malloc(sf1_info.frames * sf1_info.channels * sizeof(short));
	if (sf_readf_short(sf1, sf1_data, sf1_info.frames) != sf1_info.frames) {
		fprintf(stderr, "ERR: Couldn't read audio data: %s\n", sf_strerror(sf1));
		return EXIT_FAILURE;
	}

	if (is_silence(sf1_data, sf1_info.channels, sf1_info.frames)) {
		fprintf(stderr, "ERR: Source 1 is all silence\n");
		return EXIT_FAILURE;
	}

	short *sf2_data = malloc(sf2_info.frames * sf2_info.channels * sizeof(short));
	if (sf_readf_short(sf2, sf2_data, sf2_info.frames) != sf2_info.frames) {
		fprintf(stderr, "ERR: Couldn't read audio data: %s\n", sf_strerror(sf2));
		return EXIT_FAILURE;
	}

	if (is_silence(sf2_data, sf2_info.channels, sf2_info.frames)) {
		fprintf(stderr, "ERR: Source 2 is all silence\n");
		return EXIT_FAILURE;
	}

	/* Calculate cross-correlation between two audio streams by applying
	 * different offsets while keeping the defined minimal overlap. */

	const size_t channels = sf1_info.channels;
	const size_t sf1_frames = sf1_info.frames;
	const size_t sf2_frames = sf2_info.frames;
	const size_t min_overlap = MIN(512, MIN(sf1_frames, sf2_frames));
	const size_t cross_correlation_frames = sf1_frames + sf2_frames - min_overlap;
	long long *cross_correlation = calloc(cross_correlation_frames, sizeof(long long));

	#pragma omp parallel for schedule(dynamic)
	for (size_t i = min_overlap; i < cross_correlation_frames; i++) {

		size_t sf1_begin = i < sf2_frames ? 0 : i - sf2_frames;
		size_t sf2_begin = i < sf2_frames ? sf2_frames - i : 0;
		size_t sf1_end = i < sf1_frames ? i : sf1_frames;
		const size_t overlap = sf1_end - sf1_begin;

		long long cc_v = 0;
		for (size_t j = 0; j < overlap; j++)
			for (size_t k = 0; k < channels; k++)
				cc_v +=
					sf1_data[(sf1_begin + j) * channels + k] *
					sf2_data[(sf2_begin + j) * channels + k];

		#pragma omp atomic
		cross_correlation[i] += cc_v;

	}

	ssize_t max_i = 0;
	long long max_v = cross_correlation[0];
	/* Find the maximum value in the cross-correlation array. */
	for (size_t i = min_overlap; i < cross_correlation_frames; i++)
		if (cross_correlation[i] > max_v)
			max_v = cross_correlation[max_i = i];

	printf("Best alignment: %zd\n", max_i - sf2_frames);

	sf_close(sf1);
	sf_close(sf2);
	free(cross_correlation);
	free(sf1_data);
	free(sf2_data);

	return EXIT_SUCCESS;
}
