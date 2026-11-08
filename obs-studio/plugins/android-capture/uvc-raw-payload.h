/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* One feed is exactly one complete USB bulk payload (not an arbitrary stream
 * chunk). The caller owns frame and consumes a completed frame before feed.
 * Matching is by FID and, when both sides provide it, PTS. After a bad frame
 * the remaining payloads with that identity are discarded until it changes. */
struct uvc_raw_payload_state {
	uint8_t *frame;
	size_t expected_size, bytes;
	uint64_t bad_frames;
	uint32_t pts;
	uint8_t fid;
	bool have_identity, have_pts, active, discarding;
};
static inline void uvc_raw_payload_init(struct uvc_raw_payload_state *s, uint8_t *frame, size_t expected_size)
{
	memset(s, 0, sizeof(*s));
	s->frame = frame;
	s->expected_size = expected_size;
}
/* Call after a transport sequence gap; preserves storage and bad_frames. */
static inline void uvc_raw_payload_reset(struct uvc_raw_payload_state *s)
{
	if (s->active) {
		s->bad_frames++;
	}
	s->bytes = 0;
	s->active = false;
	s->discarding = false;
	s->have_identity = false;
	s->have_pts = false;
}
static inline void uvc_raw_payload_discard(struct uvc_raw_payload_state *s)
{
	if (!s->discarding) {
		s->bad_frames++;
	}
	s->active = false;
	s->bytes = 0;
	s->discarding = s->have_identity;
}
static inline int uvc_raw_payload_feed(struct uvc_raw_payload_state *s, const uint8_t *payload, size_t length)
{
	if (!payload || length < 2 || !s->frame || !s->expected_size) {
		uvc_raw_payload_discard(s);
		return 0;
	}
	const uint8_t hle = payload[0], bmh = payload[1];
	const bool pts_present = (bmh & 0x04) != 0;
	const unsigned expected_hle = 2U + (pts_present ? 4U : 0U) + ((bmh & 0x08) ? 6U : 0U);
	/* The USB transfer supplies the payload boundary, so EOH is not needed
	 * for synchronization. Extra header bytes are metadata, not pixels. */
	if ((bmh & 0x10) || hle < expected_hle || hle > length) {
		uvc_raw_payload_discard(s);
		return 0;
	}
	const uint8_t fid = bmh & 1U;
	const uint32_t pts = pts_present ? ((uint32_t)payload[2] | (uint32_t)payload[3] << 8 |
					    (uint32_t)payload[4] << 16 | (uint32_t)payload[5] << 24)
					 : 0;
	bool changed = s->have_identity && (fid != s->fid || (s->have_pts && pts_present && pts != s->pts));
	if (changed) {
		if (s->active) {
			s->bad_frames++; /* No EOF before the next frame. */
		}
		s->bytes = 0;
		s->active = false;
		s->discarding = false;
		s->have_identity = false;
	}
	if (!s->have_identity) {
		s->have_identity = true;
		s->fid = fid;
		s->have_pts = pts_present;
		s->pts = pts;
	} else if (!s->have_pts && pts_present) {
		s->have_pts = true;
		s->pts = pts;
	}
	if (s->discarding) {
		return 0;
	}
	if (bmh & 0x40) {
		uvc_raw_payload_discard(s);
		return 0;
	}
	s->active = true;
	size_t count = length - hle;
	if (count > s->expected_size - s->bytes) {
		uvc_raw_payload_discard(s);
		return 0;
	}
	memcpy(s->frame + s->bytes, payload + hle, count);
	s->bytes += count;
	if (!(bmh & 0x02)) {
		return 0;
	}
	if (s->bytes != s->expected_size) {
		uvc_raw_payload_discard(s);
		return 0;
	}
	s->active = false;
	s->discarding = true; /* Ignore duplicate same-frame payloads until new identity. */
	return 1;
}
