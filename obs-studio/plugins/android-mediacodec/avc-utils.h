/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Converts AVC access units with four-byte NAL lengths in place. */
static bool avc_to_annexb(uint8_t *p, size_t n)
{
	if (!p || n < 4) {
		return false;
	}
	if (p[0] == 0 && p[1] == 0 && (p[2] == 1 || (p[2] == 0 && p[3] == 1))) {
		return true;
	}
	size_t i = 0;
	while (i + 4 <= n) {
		uint32_t len = ((uint32_t)p[i] << 24) | ((uint32_t)p[i + 1] << 16) | ((uint32_t)p[i + 2] << 8) |
			       p[i + 3];
		if (!len || len > n - i - 4) {
			return false;
		}
		p[i] = p[i + 1] = p[i + 2] = 0;
		p[i + 3] = 1;
		i += 4 + len;
	}
	return i == n;
}
static bool avc_has_nal(const uint8_t *p, size_t n, unsigned type)
{
	for (size_t i = 0; i + 3 < n; i++) {
		size_t start = 0;
		if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
			start = i + 3;
		} else if (i + 4 < n && p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 0 && p[i + 3] == 1) {
			start = i + 4;
		}
		if (start && start < n && (p[start] & 31) == type) {
			return true;
		}
	}
	return false;
}

/* OBS PTS units are 1/fps_num seconds; MediaCodec uses microseconds. */
static bool avc_pts_to_ns(int64_t pts, int fps_num, int64_t *ns)
{
	if (pts < 0 || fps_num <= 0 || !ns || pts / fps_num > INT64_MAX / 1000000000LL) {
		return false;
	}
	int64_t whole = (pts / fps_num) * 1000000000LL;
	int64_t fraction = (pts % fps_num) * 1000000000LL / fps_num;
	if (whole > INT64_MAX - fraction) {
		return false;
	}
	*ns = whole + fraction;
	return true;
}
static bool avc_us_to_pts(int64_t us, int fps_num, int64_t *pts)
{
	if (us < 0 || fps_num <= 0 || !pts || us / 1000000 > INT64_MAX / fps_num) {
		return false;
	}
	int64_t whole = (us / 1000000) * fps_num;
	int64_t fraction = ((us % 1000000) * fps_num + 500000) / 1000000;
	if (whole > INT64_MAX - fraction) {
		return false;
	}
	*pts = whole + fraction;
	return true;
}

/* Read RBSP bits while removing emulation-prevention bytes. */
struct avc_bits {
	const uint8_t *p;
	size_t n, pos;
	unsigned zeros, left, value;
	bool failed;
};
static unsigned avc_bit(struct avc_bits *b)
{
	if (!b->left) {
		if (b->pos >= b->n) {
			b->failed = true;
			return 0;
		}
		unsigned c = b->p[b->pos++];
		if (b->zeros >= 2 && c == 3) {
			if (b->pos >= b->n) {
				b->failed = true;
				return 0;
			}
			c = b->p[b->pos++];
			b->zeros = 0;
		}
		b->zeros = c == 0 ? b->zeros + 1 : 0;
		b->value = c;
		b->left = 8;
	}
	return (b->value >> --b->left) & 1;
}
static unsigned avc_bits_read(struct avc_bits *b, unsigned count)
{
	unsigned v = 0;
	while (count--) {
		v = (v << 1) | avc_bit(b);
	}
	return v;
}
static unsigned avc_ue(struct avc_bits *b)
{
	unsigned z = 0;
	while (!avc_bit(b)) {
		if (b->failed || ++z > 30) {
			b->failed = true;
			return 0;
		}
	}
	return ((1u << z) - 1) + avc_bits_read(b, z);
}
/* Accept only AVC SPS with 8-bit 4:2:0 coding. */
static bool avc_sps_8bit_420(const uint8_t *p, size_t n)
{
	size_t start = n;
	for (size_t i = 0; i + 3 < n; i++) {
		size_t h = 0;
		if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
			h = i + 3;
		} else if (i + 4 < n && p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 0 && p[i + 3] == 1) {
			h = i + 4;
		}
		if (h && h < n && (p[h] & 31) == 7) {
			start = h + 1;
			break;
		}
	}
	if (start >= n) {
		return false;
	}
	size_t end = n;
	for (size_t i = start; i + 2 < n; i++) {
		if (p[i] == 0 && p[i + 1] == 0 && (p[i + 2] == 1 || (i + 3 < n && p[i + 2] == 0 && p[i + 3] == 1))) {
			end = i;
			break;
		}
	}
	struct avc_bits b = {.p = p + start, .n = end - start};
	unsigned profile = avc_bits_read(&b, 8);
	avc_bits_read(&b, 8);
	avc_bits_read(&b, 8);
	avc_ue(&b);
	if (b.failed) {
		return false;
	}
	switch (profile) {
	case 66:
	case 77:
	case 88:
		return true;
	case 44:
	case 83:
	case 86:
	case 100:
	case 110:
	case 118:
	case 122:
	case 128:
	case 134:
	case 135:
	case 138:
	case 139:
	case 144:
	case 244: {
		unsigned chroma = avc_ue(&b);
		if (chroma == 3) {
			avc_bit(&b);
		}
		unsigned luma = avc_ue(&b), chroma_depth = avc_ue(&b);
		return !b.failed && chroma == 1 && luma == 0 && chroma_depth == 0;
	}
	default:
		return false;
	}
}
