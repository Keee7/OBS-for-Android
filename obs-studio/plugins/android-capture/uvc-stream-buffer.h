/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Consuming a payload advances a view. Only compact retained bytes when the
 * next USB block cannot fit at the physical end of the allocation. */
struct uvc_stream_buffer {
	uint8_t *storage;
	uint8_t *data;
	size_t capacity;
	size_t size;
	uint64_t compacted_bytes;
};

static inline void uvc_stream_buffer_reset(struct uvc_stream_buffer *b)
{
	b->data = b->storage;
	b->size = 0;
}

static inline void uvc_stream_buffer_consume(struct uvc_stream_buffer *b, size_t count)
{
	if (count >= b->size) {
		uvc_stream_buffer_reset(b);
	} else {
		b->data += count;
		b->size -= count;
	}
}

static inline bool uvc_stream_buffer_append(struct uvc_stream_buffer *b, const uint8_t *data, size_t size)
{
	if (size > b->capacity - b->size) {
		return false;
	}
	if (!size) {
		return true;
	}
	const size_t offset = (size_t)(b->data - b->storage);
	if (size > b->capacity - offset - b->size) {
		memmove(b->storage, b->data, b->size);
		b->compacted_bytes += b->size;
		b->data = b->storage;
	}
	memcpy(b->data + b->size, data, size);
	b->size += size;
	return true;
}
