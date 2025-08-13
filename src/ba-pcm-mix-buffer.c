/*
 * BlueALSA - ba-mix-buffer.c
 * Copyright (c) 2016-2025 Arkadiusz Bokowy
 * Copyright (c) 2025 borine
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <endian.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ba-transport-pcm.h"
#include "shared/log.h"
#include "ba-pcm-mix-buffer.h"
#include "ba-pcm-multi.h"

#define BA_24BIT_MIN (int32_t)0xFF800000
#define BA_24BIT_MAX (int32_t)0x007FFFFF

#if __BYTE_ORDER == __LITTLE_ENDIAN

#define BA_S16_2LE_TO_INT32(x) (x)
#define BA_S32_4LE_TO_INT64(x) (x)
#define BA_S24_4LE_TO_INT32(x) (int32_t)((x & 0x00800000) ? x | 0xFF000000 : x)
#define BA_INT32_TO_S24_4LE(x) (uint32_t)(((uint32_t)x & 0x80000000) >> 8) | ((uint32_t)x & 0x007FFFFF)

#elif __BYTE_ORDER == __BIG_ENDIAN

#include <byteswap.h>
#define BA_S16_2LE_TO_INT32(x) (int16_t)bswap_16(x)
#define BA_S32_4LE_TO_INT64(x) (int32_t)bswap_32(x)

#define BA_S24_LE_TO_INT32(x) ( \
	((uint32_t)x & 0xFF000000) >> 24 | \
	((uint32_t)x & 0x00FF0000) >> 8 | \
	((uint32_t)x & 0x0000FF00) << 8 | \
	((x & 0x00008000) ? 0xFF : 0x00))

#define BA_INT32_TO_S24_4LE(x) ( \
	((x & 0x80000000) ? 0xFF : 0x00)) | \
	((uint32_t)x & 0x00FF0000) >> 8 | \
	((uint32_t)x & 0x0000FF00) << 8 | \
	((uint32_t)x & 0x000000FF) << 24

#else
# error "Unknown byte order"
#endif

/**
 * Configure the mix buffer for use with given transport stream parameters.
 *
 * @param buffer Pointer to the buffer that is to be configured.
 * @param format The sample format that will be used.
 * @param channels The number of channels in each frame.
 * @param buffer_frames The requested capacity of the buffer, in frames.
 * @param period_frames The number of frames to be transferred at one time.*/
int ba_pcm_mix_buffer_init(struct ba_mix_buffer *buffer,
				uint16_t format, uint8_t channels,
				size_t buffer_frames, size_t period_frames) {
	buffer->format = format;
	buffer->channels = channels;
	buffer->size = buffer_frames * channels;
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
		case BA_TRANSPORT_PCM_FORMAT_S24_4LE:
			buffer->frame_size = channels * sizeof(int32_t);
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
void ba_pcm_mix_buffer_release(struct ba_mix_buffer *buffer) {
	buffer->size = 0;
	free(buffer->data.any);
	buffer->data.any = NULL;
}

/**
 * The number of samples that can be read from start offset to end offset.
 *
 * @param start offset of first sample to be read.
 * @param end   offset of sample after the last one to be read. */
size_t ba_pcm_mix_buffer_calc_avail(const struct ba_mix_buffer *buffer, size_t start, size_t end) {
	if (end >= start)
		return end - start;
	else
		return buffer->size + end - start;
}

/**
 * Is the buffer empty ? */
bool ba_pcm_mix_buffer_empty(const struct ba_mix_buffer *buffer) {
	return buffer->mix_offset == buffer->end;
}

/**
 * The delay, expressed in samples, that would be incurred by adding the next
 * frame at the given offset. */
size_t ba_pcm_mix_buffer_delay(const struct ba_mix_buffer *buffer, size_t offset) {
	return ba_pcm_mix_buffer_calc_avail(buffer, buffer->mix_offset, offset);
}

/**
 * true if the number of frames available to be read is greater than the
 * start threshold. */
bool ba_pcm_mix_buffer_at_threshold(struct ba_mix_buffer *buffer) {
	size_t avail = ba_pcm_mix_buffer_calc_avail(buffer, buffer->mix_offset, buffer->end);
	return avail >= BA_MULTI_MIX_THRESHOLD * buffer->period;
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
 *         a whole number of frames. */
size_t ba_pcm_mix_buffer_add(struct ba_mix_buffer *restrict buffer, intmax_t *restrict offset, const void *restrict data, size_t bytes) {

	size_t start;
	size_t mix_offset = buffer->mix_offset;
	/* Save the initial buffer fill level so that we can detect if this
	 * addition has increased it. */
	size_t avail = ba_pcm_mix_buffer_calc_avail(buffer, mix_offset, buffer->end);

	/* Only allow complete frames into the mix. */
	size_t frames = bytes / buffer->frame_size;
	size_t samples = frames * buffer->channels;

	/* convert relative offset to absolute value */
	if (*offset < 0) {
		if (-*offset > (intmax_t)buffer->size)
			*offset = mix_offset - 1;
		else
			*offset = mix_offset - *offset;
	}

	start = *offset;
	if (start >= buffer->size)
		start %= buffer->size;

	if (start < mix_offset)
		start += buffer->size;

	/* To keep all clients as closely synchronized as possible, do not
	 * allow any client to advance more than the mix threshold ahead of
	 * the current read position. */
	const size_t limit = mix_offset + BA_MULTI_MIX_THRESHOLD * buffer->period;
	if (start >= limit)
		return 0;

	if (start + samples > limit)
		samples = limit - start;

	for (size_t n = 0; n < samples; n++) {
		if (start + n >= buffer->size)
			start -= buffer->size;
		switch (buffer->format) {
			case BA_TRANSPORT_PCM_FORMAT_U8: {
				uint8_t *ptr = (uint8_t*)data;
				buffer->data.s16[start + n] += ptr[n] - 0x80;
				break;
			}
			case BA_TRANSPORT_PCM_FORMAT_S16_2LE: {
				const int16_t *ptr = (const int16_t*)data;
				buffer->data.s32[start + n] += BA_S16_2LE_TO_INT32(ptr[n]);
				break;
			}
			case BA_TRANSPORT_PCM_FORMAT_S24_4LE: {
				const uint32_t *ptr = (const uint32_t*)data;
				buffer->data.s32[start + n] += BA_S24_4LE_TO_INT32(ptr[n]);
				break;
			}
			case BA_TRANSPORT_PCM_FORMAT_S32_4LE: {
				const int32_t *ptr = (const int32_t*)data;
				buffer->data.s64[start + n] += BA_S32_4LE_TO_INT64(ptr[n]);
				break;
			}
		}
	}

	start += samples;
	if (start >= buffer->size)
		start -= buffer->size;
	*offset = start;

	/* If this addition has increased the number of available frames, update
	 * the end pointer. */
	if (ba_pcm_mix_buffer_calc_avail(buffer, mix_offset, *offset) > avail)
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
 * @param frames Size of the data buffer in samples.
 * @param scale An array of scaling factors, one for each channel of the stream.
 * @return number of samples fetched from mix. This is always complete frames.
 * */
size_t ba_pcm_mix_buffer_read(struct ba_mix_buffer *restrict buffer, void *restrict data, size_t samples, double *restrict scale) {

	size_t start = buffer->mix_offset;
	size_t end = buffer->end;
	/* Only process complete frames */
	samples -= samples % buffer->channels;

	/* Limit each read to 1 period. */
	if (samples > buffer->period)
		samples = buffer->period;

	/* Do not read beyond the last sample written. */
	size_t avail = ba_pcm_mix_buffer_calc_avail(buffer, start, end);
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
			case BA_TRANSPORT_PCM_FORMAT_S24_4LE: {
				uint32_t *dest = (uint32_t*) data;
				int32_t *sample = buffer->data.s32 + start + n;
				int channel;
				for (channel = 0; channel < buffer->channels; channel++) {
					if (scale[channel] == 0.0) {
						sample[channel] = 0;
					}
					else {
						sample[channel] *= scale[channel];
						if (sample[channel] > BA_24BIT_MAX)
							sample[channel] = BA_24BIT_MAX;
						else if (sample[channel] < BA_24BIT_MIN)
							sample[channel] = BA_24BIT_MIN;
					}
					dest[out_offset++] =
                       (uint32_t)BA_INT32_TO_S24_4LE(sample[channel]);
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

	start += n;
	if (start >= buffer->size)
		start -= buffer->size;
	buffer->mix_offset = start;

	return samples;
}

/**
 * Discard all frames from the mix buffer. */
void ba_pcm_mix_buffer_clear(struct ba_mix_buffer *buffer) {
	buffer->mix_offset = 0;
	buffer->end = 0;
	size_t buffer_bytes;
	switch(buffer->format) {
		case BA_TRANSPORT_PCM_FORMAT_U8:
			buffer_bytes = buffer->size * sizeof(int16_t);
			break;
		case BA_TRANSPORT_PCM_FORMAT_S16_2LE:
		case BA_TRANSPORT_PCM_FORMAT_S24_4LE:
			buffer_bytes = buffer->size * sizeof(int32_t);
			break;
		case BA_TRANSPORT_PCM_FORMAT_S32_4LE:
			buffer_bytes = buffer->size * sizeof(int64_t);
			break;
		default:
			/* not reached */
			buffer_bytes = 0;
	}
	memset(buffer->data.any, 0, buffer_bytes);
}
