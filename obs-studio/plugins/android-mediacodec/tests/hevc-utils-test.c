/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../hevc-utils.h"
#include <assert.h>
#include <string.h>

/* Synthetic SPS prefixes with real HEVC syntax through the depth fields. */
struct writer { uint8_t p[128]; size_t bits; };
static void put(struct writer *w, unsigned value, unsigned n)
{
	while (n--) {
		if ((value >> n) & 1) w->p[w->bits / 8] |= 0x80 >> (w->bits % 8);
		++w->bits;
	}
}
static void ue(struct writer *w, unsigned v)
{
	unsigned k = 0, x = v + 1;
	for (unsigned t = x; t > 1; t >>= 1) ++k;
	put(w, 0, k); put(w, x, k + 1);
}
static void ptl(struct writer *w, unsigned idc, unsigned compatible_main)
{
	put(w, 0, 3); put(w, idc, 5); /* profile_space and tier */
	put(w, (1u << (31 - idc)) | (compatible_main ? 1u << 30 : 0), 32);
	put(w, 0, 16); put(w, 0, 32); /* constraints */
	put(w, 120, 8); /* level */
}
static size_t sample(uint8_t *out, unsigned chroma, unsigned depth, unsigned layers,
		     unsigned profile, unsigned compatible_main)
{
	struct writer w = {0};
	put(&w, 0, 4); put(&w, layers, 3); put(&w, 1, 1);
	ptl(&w, profile, compatible_main);
	for (unsigned i = 0; i < layers; ++i) { put(&w, 1, 1); put(&w, 1, 1); }
	if (layers) for (unsigned i = layers; i < 8; ++i) put(&w, 0, 2);
	for (unsigned i = 0; i < layers; ++i) ptl(&w, 1, 1);
	ue(&w, 0); ue(&w, chroma); if (chroma == 3) put(&w, 0, 1);
	ue(&w, 64); ue(&w, 64); put(&w, 0, 1); ue(&w, depth); ue(&w, depth);
	put(&w, 1, 1); /* trailing stop bit */
	out[0] = out[1] = 0; out[2] = 1; out[3] = 0x42; out[4] = 1;
	size_t n = 5, zeros = 0;
	for (size_t i = 0; i < (w.bits + 7) / 8; ++i) {
		uint8_t c = w.p[i];
		if (zeros >= 2 && c <= 3) { out[n++] = 3; zeros = 0; }
		out[n++] = c; zeros = c == 0 ? zeros + 1 : 0;
	}
	return n;
}
int main(void)
{
	/* SPS excerpt from Chromium's bear.hevc, via WebRTC (BSD-3-Clause):
	 * https://webrtc.googlesource.com/src/+/refs/heads/main/common_video/h265/h265_bitstream_parser_unittest.cc */
	const uint8_t real[] = {0,0,0,1,0x42,0x01,0x01,0x01,0x60,0,0,3,0,0x80,0,0,3,0,0,3,0,0x3c,0xa0,0x0a,0x08,0x0b,0x9f,0x79,0x65,0x79,0x24,0xca,0xe0,0x10,0,0,3,0,0x10,0,0,3,1,0xe0,0x80};
	assert(hevc_sps_8bit_420(real, sizeof real));
	uint8_t sps[256]; size_t n = sample(sps, 1, 0, 0, 1, 1);
	assert(hevc_sps_8bit_420(sps, n));
	for (size_t i = 0; i < n - 1; ++i) assert(!hevc_sps_8bit_420(sps, i));
	n = sample(sps, 1, 2, 0, 1, 1); assert(!hevc_sps_8bit_420(sps, n));
	n = sample(sps, 2, 0, 0, 1, 1); assert(!hevc_sps_8bit_420(sps, n));
	n = sample(sps, 1, 0, 0, 2, 0); assert(!hevc_sps_8bit_420(sps, n));
	n = sample(sps, 1, 0, 0, 2, 1); assert(hevc_sps_8bit_420(sps, n));
	sps[6] |= 0x40; assert(!hevc_sps_8bit_420(sps, n)); /* nonzero profile_space */
	n = sample(sps, 1, 0, 2, 1, 1); assert(hevc_sps_8bit_420(sps, n));
	uint8_t headers[] = {0,0,0,1,0x40,1,0x80, 0,0,1,0x42,1,0x80, 0,0,0,1,0x44,1,0x80};
	assert(hevc_headers_ready(headers, sizeof headers));
	for (unsigned type = 32; type <= 34; ++type) assert(hevc_has_nal(headers, sizeof headers, type));
	assert(!hevc_has_nal(headers, sizeof headers, 64));
	assert(!hevc_headers_ready(headers, sizeof headers - 1));
	uint8_t short_nal[] = {0,0,1,0x40,1};
	assert(!hevc_has_nal(short_nal, sizeof short_nal, 32));
	headers[4] |= 0x80; assert(!hevc_headers_ready(headers, sizeof headers)); headers[4] &= 0x7f;
	headers[10] = 0; assert(!hevc_headers_ready(headers, sizeof headers));
	uint8_t lengths[] = {0,0,0,3,0x40,1,0x80,0,0,0,3,0x44,1,0x80};
	assert(avc_to_annexb(lengths, sizeof lengths));
	assert(hevc_has_nal(lengths, sizeof lengths, 32) && hevc_has_nal(lengths, sizeof lengths, 34));
	return 0;
}
