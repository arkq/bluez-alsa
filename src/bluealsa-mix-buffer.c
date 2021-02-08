/*
 * BlueALSA - bluealsa-mix-buffer.c
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ba-transport.h"
#include "shared/log.h"
#include "bluealsa-mix-buffer.h"


/**
 * Configure the mix buffer for use with given transport stream parameters.
 *
 * @param buffer Pointer to the buffer that is to be configured.
 * @param format The sample format that will be used.
 * @param channels The number of channels in each frame.
 * @param buffer_frames The requested capacity of the buffer, in frames.
 * @param period_frames The number of frames to be transferred at one time.*/
int bluealsa_mix_buffer_init(struct bluealsa_mix_buffer *buffer,
                                  uint16_t format, uint8_t channels,
                                  size_t buffer_frames, size_t period_frames) {
	buffer->format = format;
	buffer->channels = channels;
	/* We allow for 1 extra empty frame in the buffer. */
	buffer->size = (1 + buffer_frames) * channels;
	buffer->period = period_frames * channels;
	buffer->mix_offset = 0;
	buffer->end = 0;
	switch(format) {
		case BA_TRANSPORT_PCM_FORMAT_U8:
			buffer->frame_size = channels * sizeof(uint8_t);
			buffer->data.s16 = calloc(buffer->size, sizeof(int16_t));
			break;
		case BA_TRANSPORT_PCM_FORMAT_S16_2LE:
			buffer->frame_size = channels * sizeof(int16_t);
			buffer->data.s32 = calloc(buffer->size, sizeof(int32_t));
			break;
		case BA_TRANSPORT_PCM_FORMAT_S32_4LE:
			buffer->frame_size = channels * sizeof(int32_t);
			buffer->data.s64 = calloc(buffer->size, sizeof(int64_t));
			break;
		default:
			error("Invalid format %u", format);
			return -1;
			break;
	}
	if (buffer->data.any == NULL) {
		error("Out of memory");
		return -1;
	}
	return 0;
}

/**
 * Release the resources used by a mix buffer. */
void bluealsa_mix_buffer_release(struct bluealsa_mix_buffer *buffer) {
	buffer->size = 0;
	free(buffer->data.any);
	buffer->data.any = NULL;
}

/**
 * The number of samples that can be read from start offset to end offset.
 *
 * @param start offset of first sample to be read.
 * @param end   offset of last sample to be read. */
static size_t bluealsa_mix_buffer_calc_avail(
                struct bluealsa_mix_buffer *buffer, size_t start, size_t end) {
	if (end >= start)
		return end - start;
	else
		return buffer->size + end - start;
}

/**
 * The number of samples that can be added between start offset and end offset.
 *
 * @param start offset for first sample to be written.
 * @param end   offset of mix read pointer. */
static size_t bluealsa_mix_buffer_calc_space(
                struct bluealsa_mix_buffer *buffer, size_t start, size_t end) {
	size_t unused;
	if (end > start)
		unused = end - start;
	else
		unused = buffer->size + end - start;

	/* Do not fill the last frame of the buffer - otherwise it is impossible to
	 * differentiate empty and full. */
	if (unused <= buffer->channels)
		unused = 0;

	return unused;
}

/**
 * The maximum number of samples that can be read from the mix. */
size_t bluealsa_mix_buffer_avail(struct bluealsa_mix_buffer *buffer) {
	return bluealsa_mix_buffer_calc_avail(buffer,
                                              buffer->mix_offset, buffer->end);
}

/**
 * Is the buffer empty ? */
bool bluealsa_mix_buffer_empty(struct bluealsa_mix_buffer *buffer) {
	return bluealsa_mix_buffer_avail(buffer) == 0;
}

/**
 * The delay, expressed in samples, that would be incurred by adding the next
 * frame at the given offset. */
size_t bluealsa_mix_buffer_delay(struct bluealsa_mix_buffer *buffer,
                                                               size_t offset) {
	return bluealsa_mix_buffer_calc_avail(buffer, buffer->mix_offset, offset);
}

/**
 * Clear a mix buffer, deleting any obsolete data but retaining the same
 * configuration. */
void bluealsa_mix_buffer_reset(struct bluealsa_mix_buffer *buffer) {
	buffer->mix_offset = 0;
	buffer->end = 0;
	memset(buffer->data.any, 0,
                buffer->size * BA_TRANSPORT_PCM_FORMAT_BYTES(buffer->format));
}

/**
 * Add a stream of bytes from a client into the mix.
 *
 * @param offset Current position of this client in the mix buffer. To be
 *        stored between calls. A negative value is interpreted as relative to
 *        (ahead of) the current mix offset.
 * @param data Pointer to the byte stream.
 * @param bytes The number of bytes in the stream.
 * @return The number of bytes actually added into the mix. This value is always
 *         a whole number of frames. A negative value indicates underrun. */
ssize_t bluealsa_mix_buffer_add(struct bluealsa_mix_buffer *buffer,
                                   ssize_t *offset, void *data, size_t bytes) {

	size_t mix_offset = buffer->mix_offset;

	size_t start;
	if  (*offset < 0) {
		start = mix_offset - *offset;
	}
	else
		start = *offset;

	/* Only allow complete frames into the mix. */
	size_t frames = bytes / buffer->frame_size;
	ssize_t samples = frames * buffer->channels;

	/* Limit the transfer to 1 period. */
	if (samples > buffer->period)
		samples = buffer->period;

	/* Limit the transfer if there is not sufficient space. */
	size_t space = bluealsa_mix_buffer_calc_space(buffer, start, mix_offset);
	if (space == 0)
		return 0;
	if (samples > space)
		samples = space;

	size_t count = samples;

	ssize_t base = start;
	size_t n;
	for (n = 0; n < count; n++) {
		if (base + n >= buffer->size)
			base -= buffer->size;
		switch (buffer->format) {
			case BA_TRANSPORT_PCM_FORMAT_U8: {
				uint8_t *ptr = (uint8_t*)data;
				buffer->data.s16[base + n] += ptr[n] - 0x80;
				break;
			}
			case BA_TRANSPORT_PCM_FORMAT_S16_2LE: {
				int16_t *ptr = (int16_t*)data;
				buffer->data.s32[base + n] += (int16_t)le16toh(ptr[n]);
				break;
			}
			case BA_TRANSPORT_PCM_FORMAT_S32_4LE: {
				int32_t *ptr = (int32_t*)data;
				buffer->data.s64[base + n] += (int32_t)le32toh(ptr[n]);
				break;
			}
		}
	}

	*offset = base + n;

	if (buffer->end == mix_offset ||
	       (start <= buffer->end && start + count > buffer->end) ||
	       (start > buffer->end && start + count > buffer->end + buffer->size))
		buffer->end = *offset;

	/* return number of bytes consumed from client */
	return samples * buffer->frame_size / buffer->channels;
}

/**
 * Read mixed frames from the mix buffer.
 *
 * Applies volume scaling to the samples returned.
 *
 * @param data Pointer to a buffer in which to place the frames.
 * @param samples Size of the data buffer in samples.
 * @param scale An array of scaling factors, one for each channel of the stream.
 * @return number of samples fetched from mix. This is always complete frames.
 * */
size_t bluealsa_mix_buffer_read(struct bluealsa_mix_buffer *buffer,
                                   void *data, size_t samples, double *scale) {

	size_t start = buffer->mix_offset;
	size_t end = buffer->end;
	samples -= samples % buffer->channels;

	/* Limit each read to 1 period. */
	if (samples > buffer->period)
		samples = buffer->period;

	/* Do not read beyond the last sample written. */
	size_t avail = bluealsa_mix_buffer_calc_avail(buffer, start, end);
	if (samples > avail)
		samples = avail;

	size_t out_offset = 0;
	size_t n;
	for (n = 0; n < samples; n += buffer->channels) {
		if (start + n >= buffer->size)
			start -= buffer->size;
		switch (buffer->format) {
			case BA_TRANSPORT_PCM_FORMAT_U8: {
				uint8_t *dest = (uint8_t*) data;
				int16_t *sample = buffer->data.s16 + start + n;
				int channel;
				for (channel = 0; channel < buffer->channels; channel++) {
					if (scale[channel] == 0.0) {
						sample[channel] = 0;
					}
					else {
						sample[channel] *= scale[channel];
						if (sample[channel] > INT8_MAX)
							sample[channel] = INT8_MAX;
						else if (sample[channel] < INT8_MIN)
							sample[channel] = INT8_MIN;
					}
					dest[out_offset++] =
                               0x80 + (int8_t)htole16((uint8_t)sample[channel]);
					sample[channel] = 0;
				}
				break;
			}
			case BA_TRANSPORT_PCM_FORMAT_S16_2LE: {
				int16_t *dest = (int16_t*) data;
				int32_t *sample = buffer->data.s32 + start + n;
				int channel;
				for (channel = 0; channel < buffer->channels; channel++) {
					if (scale[channel] == 0.0) {
						sample[channel] = 0;
					}
					else {
						if (scale[channel] < 0.99)
							sample[channel] *= scale[channel];
						if (sample[channel] > INT16_MAX)
							sample[channel] = INT16_MAX;
						else if (sample[channel] < INT16_MIN)
							sample[channel] = INT16_MIN;
					}
					dest[out_offset++] =
                                    (int16_t)htole16((uint16_t)sample[channel]);
					sample[channel] = 0;
				}
				break;
			}
			case BA_TRANSPORT_PCM_FORMAT_S32_4LE: {
				int32_t *dest = (int32_t*) data;
				int64_t *sample = buffer->data.s64 + start + n;
				int channel;
				for (channel = 0; channel < buffer->channels; channel++) {
					if (scale[channel] == 0.0) {
						sample[channel] = 0;
					}
					else {
						sample[channel] *= scale[channel];
						if (sample[channel] > INT32_MAX)
							sample[channel] = INT32_MAX;
						else if (sample[channel] < INT32_MIN)
							sample[channel] = INT32_MIN;
					}
					dest[out_offset++] =
                                    (int32_t)htole32((uint32_t)sample[channel]);
					sample[channel] = 0;
				}
				break;
			}
		}
	}

	buffer->mix_offset = start + n;

	return samples;
}

