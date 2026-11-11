// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <stddef.h>
#include <stdint.h>

/* MediaCodec may supply an av1C record or low-overhead OBUs. Retain only
 * the sequence header, leaving any frame data in its original packet. */
static const uint8_t *av1_sequence_header(const uint8_t *data, size_t size, size_t *header_size)
{
	if (size >= 4 && data[0] == 0x81) {
		data += 4;
		size -= 4;
	}
	for (size_t offset = 0; offset < size;) {
		size_t start = offset;
		uint8_t header = data[offset++];
		if ((header & 0x81) || !(header & 0x02))
			return NULL;
		if (header & 0x04) {
			if (offset == size)
				return NULL;
			++offset;
		}
		uint64_t length = 0;
		uint8_t byte;
		unsigned shift = 0;
		do {
			if (offset == size || shift == 56)
				return NULL;
			byte = data[offset++];
			length |= (uint64_t)(byte & 0x7f) << shift;
			shift += 7;
		} while (byte & 0x80);
		if (length > size - offset)
			return NULL;
		offset += (size_t)length;
		if (((header >> 3) & 0xf) == 1 && length != 0) {
			*header_size = offset - start;
			return data + start;
		}
	}
	return NULL;
}
