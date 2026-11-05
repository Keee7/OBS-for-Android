/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "avc-utils.h"

/* Find a NAL delimited by either Annex B start-code length. */
static bool hevc_next_nal(const uint8_t *p, size_t n, size_t *cursor, size_t *start, size_t *end)
{
	if (!p || !cursor || *cursor > n)
		return false;
	size_t i = *cursor;
	while (i + 3 <= n && !(p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1))
		++i;
	if (i + 3 > n)
		return false;
	*start = i + 3;
	i = *start;
	while (i + 3 <= n && !(p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1))
		++i;
	*end = i + 3 <= n ? i : n;
	/* A four-byte prefix's first zero is trailing_zero_8bits. */
	while (*end > *start && p[*end - 1] == 0)
		--*end;
	*cursor = i + 3 <= n ? i : n;
	return true;
}
static bool hevc_valid_nal(const uint8_t *p, size_t n)
{
	return n >= 3 && !(p[0] & 0x80) && (p[1] & 7) != 0;
}
static bool hevc_has_nal(const uint8_t *p, size_t n, unsigned type)
{
	size_t cursor = 0, start, end;
	if (type > 63)
		return false;
	while (hevc_next_nal(p, n, &cursor, &start, &end)) {
		if (hevc_valid_nal(p + start, end - start) && ((p[start] >> 1) & 63) == type)
			return true;
	}
	return false;
}
static bool hevc_headers_ready(const uint8_t *p, size_t n)
{
	return hevc_has_nal(p, n, 32) && hevc_has_nal(p, n, 33) && hevc_has_nal(p, n, 34);
}
struct hevc_bits {
	const uint8_t *p;
	size_t n, pos;
	unsigned zeros, left, value;
	bool failed;
};
static unsigned hevc_bit(struct hevc_bits *b)
{
	if (!b->left) {
		if (b->pos >= b->n) {
			b->failed = true;
			return 0;
		}
		unsigned c = b->p[b->pos++];
		if (b->zeros >= 2 && c == 3) {
			if (b->pos >= b->n || b->p[b->pos] > 3) {
				b->failed = true;
				return 0;
			}
			c = b->p[b->pos++];
			b->zeros = 0;
		} else if (b->zeros >= 2 && c <= 2) {
			b->failed = true;
			return 0;
		}
		b->zeros = c == 0 ? b->zeros + 1 : 0;
		b->value = c;
		b->left = 8;
	}
	return (b->value >> --b->left) & 1;
}
static unsigned hevc_read(struct hevc_bits *b, unsigned count)
{
	unsigned v = 0;
	while (count--)
		v = (v << 1) | hevc_bit(b);
	return v;
}
static void hevc_skip(struct hevc_bits *b, unsigned count)
{
	while (count-- && !b->failed)
		hevc_bit(b);
}
static unsigned hevc_ue(struct hevc_bits *b)
{
	unsigned zeros = 0;
	while (!hevc_bit(b)) {
		if (b->failed || ++zeros > 30) {
			b->failed = true;
			return 0;
		}
	}
	return ((1u << zeros) - 1) + hevc_read(b, zeros);
}
/* Parse only through the chroma and bit-depth fields, not the full SPS. */
static bool hevc_sps_8bit_420(const uint8_t *p, size_t n)
{
	size_t cursor = 0, start, end;
	while (hevc_next_nal(p, n, &cursor, &start, &end)) {
		if (!hevc_valid_nal(p + start, end - start) || ((p[start] >> 1) & 63) != 33)
			continue;
		/* This parser handles single-layer HEVC SPS syntax. */
		if ((p[start] & 1) || (p[start + 1] & 0xf8))
			return false;
		struct hevc_bits b = {.p = p + start + 2, .n = end - start - 2};
		hevc_skip(&b, 4);
		unsigned layers = hevc_read(&b, 3);
		hevc_skip(&b, 1);
		if (layers > 6)
			return false;
		unsigned profile_space = hevc_read(&b, 2);
		hevc_skip(&b, 1); /* general_tier_flag */
		unsigned profile_idc = hevc_read(&b, 5);
		unsigned compatibility = hevc_read(&b, 32);
		/* HEVC Annex A profile numbers apply to profile_space zero.
		 * Flag[1] is the second bit in bitstream order (bit 30). */
		if (b.failed || profile_space != 0 ||
		    (profile_idc != 1 && !(compatibility & (1u << 30))))
			return false;
		hevc_skip(&b, 56); /* remaining constraints and general_level_idc */
		unsigned profile[7] = {0}, level[7] = {0};
		for (unsigned i = 0; i < layers; ++i) {
			profile[i] = hevc_bit(&b);
			level[i] = hevc_bit(&b);
		}
		if (layers)
			hevc_skip(&b, (8 - layers) * 2);
		for (unsigned i = 0; i < layers; ++i) {
			if (profile[i]) hevc_skip(&b, 88);
			if (level[i]) hevc_skip(&b, 8);
		}
		unsigned id = hevc_ue(&b), chroma = hevc_ue(&b);
		if (chroma == 3) hevc_skip(&b, 1);
		unsigned width = hevc_ue(&b), height = hevc_ue(&b);
		if (hevc_bit(&b))
			for (unsigned i = 0; i < 4; ++i) hevc_ue(&b);
		unsigned luma = hevc_ue(&b), chroma_depth = hevc_ue(&b);
		return !b.failed && id <= 15 && chroma == 1 && width && height && luma == 0 && chroma_depth == 0;
	}
	return false;
}
