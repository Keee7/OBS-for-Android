/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "uvc-stream-buffer.h"
#include "uvc-raw-payload.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static void window_test(void)
{
	uint8_t storage[32], input[64];
	for (unsigned i = 0; i < sizeof(input); i++)
		input[i] = (uint8_t)i;
	struct uvc_stream_buffer b = {.storage = storage, .data = storage, .capacity = sizeof(storage)};
	assert(uvc_stream_buffer_append(&b, input, 20));
	uvc_stream_buffer_consume(&b, 10);
	assert(b.data == storage + 10 && b.compacted_bytes == 0);
	assert(uvc_stream_buffer_append(&b, input + 20, 15));
	assert(b.size == 25 && b.compacted_bytes == 10 && !memcmp(b.data, input + 10, 25));
	assert(!uvc_stream_buffer_append(&b, input, 8));
	assert(b.size == 25 && !memcmp(b.data, input + 10, 25));
	uvc_stream_buffer_consume(&b, 100);
	assert(b.data == storage && b.size == 0);
}

static void fragmented_window_test(void)
{
	const size_t capacity = 128 * 1024;
	uint8_t *storage = malloc(capacity), *input = malloc(65536);
	assert(storage && input);
	struct uvc_stream_buffer b = {.storage = storage, .data = storage, .capacity = capacity};
	uint64_t offset = 0, consumed = 0, old_moved = 0;
	for (unsigned block = 0; block < 128; block++) {
		const size_t count = block % 2 ? 65536 : 65521;
		for (size_t i = 0; i < count; i++)
			input[i] = (uint8_t)((offset + i) * 17U);
		assert(uvc_stream_buffer_append(&b, input, count));
		offset += count;
		while (b.size >= 1036) {
			for (size_t i = 0; i < 1036; i++)
				assert(b.data[i] == (uint8_t)((consumed + i) * 17U));
			old_moved += b.size - 1036;
			uvc_stream_buffer_consume(&b, 1036);
			consumed += 1036;
		}
	}
	assert(consumed + b.size == offset);
	assert(b.compacted_bytes < offset && old_moved > offset * 10);
	printf("PASS: sliding window retained-byte moves=%llu; former per-payload moves=%llu (synthetic stream)\n",
	       (unsigned long long)b.compacted_bytes, (unsigned long long)old_moved);
	free(input);
	free(storage);
}

static void raw_1080_test(size_t frame_size, const char *format)
{
	uint8_t *frame = malloc(frame_size), *expected = malloc(frame_size);
	uint8_t payload[32768];
	assert(frame && expected);
	for (size_t i = 0; i < frame_size; i++)
		expected[i] = (uint8_t)(i * 29U + 12U);
	/* Embed fake headers in pixels; packet framing must leave them untouched. */
	for (size_t i = 64; i + 12 < frame_size; i += 1024) {
		expected[i] = 12;
		expected[i + 1] = 0xcc;
	}
	struct uvc_raw_payload_state state;
	uvc_raw_payload_init(&state, frame, frame_size);
	for (unsigned n = 0; n < 4; n++) {
		size_t offset = 0;
		unsigned completed = 0;
		while (offset < frame_size) {
			size_t count = frame_size - offset;
			if (count > sizeof(payload) - 12)
				count = sizeof(payload) - 12;
			payload[0] = 12;
			payload[1] = 0x8c | (n & 1) | (offset + count == frame_size ? 2 : 0);
			memset(payload + 2, 0, 10);
			payload[2] = (uint8_t)n;
			memcpy(payload + 12, expected + offset, count);
			completed += uvc_raw_payload_feed(&state, payload, count + 12);
			offset += count;
		}
		assert(completed == 1 && state.bad_frames == 0 && !memcmp(frame, expected, frame_size));
	}
	printf("PASS: four complete 1920x1080 %s frames preserve all pixels, including fake headers\n", format);
	free(expected);
	free(frame);
}

int main(void)
{
	window_test();
	fragmented_window_test();
	raw_1080_test(1920U * 1080U * 2U, "YUYV");
	raw_1080_test(1920U * 1080U * 3U / 2U, "NV12");
	return 0;
}
