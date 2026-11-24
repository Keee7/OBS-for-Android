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
	uint64_t good_frames;
	uint64_t invalid_headers;
	uint64_t err_packets;
	uint64_t size_errors;
	uint64_t fed_bytes;
	uint64_t bytes_since_good;
	uint64_t non_err_bytes_since_good;
	uint32_t consec_invalid_headers;
	uint32_t consec_size_errors;
	uint32_t pts;
	uint8_t fid;
	bool have_identity, have_pts, active, discarding, completed;
};
static inline void uvc_raw_payload_init(struct uvc_raw_payload_state *s, uint8_t *frame, size_t expected_size)
{
	memset(s, 0, sizeof(*s));
	s->frame = frame;
	s->expected_size = expected_size;
}
/* Call after a transport sequence gap; preserves storage, bad_frames, and fallback counters. */
static inline void uvc_raw_payload_reset(struct uvc_raw_payload_state *s)
{
	if (s->active) {
		s->bad_frames++;
	}
	s->bytes = 0;
	s->active = false;
	s->discarding = false;
	s->completed = false;
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
	if (!s || !s->frame || !s->expected_size) {
		return 0;
	}
	if (!payload || length < 2) {
		s->fed_bytes += length;
		s->bytes_since_good += length;
		s->non_err_bytes_since_good += length;
		s->invalid_headers++;
		s->consec_invalid_headers++;
		uvc_raw_payload_discard(s);
		return 0;
	}
	s->fed_bytes += length;
	s->bytes_since_good += length;
	const uint8_t hle = payload[0], bmh = payload[1];
	const bool pts_present = (bmh & 0x04) != 0;
	const unsigned expected_hle = 2U + (pts_present ? 4U : 0U) + ((bmh & 0x08) ? 6U : 0U);
	/* The USB transfer supplies the payload boundary, so EOH is not needed
	 * for synchronization. Extra header bytes are metadata, not pixels. */
	if ((bmh & 0x10) || hle < expected_hle || hle > length) {
		s->non_err_bytes_since_good += length;
		s->invalid_headers++;
		s->consec_invalid_headers++;
		uvc_raw_payload_discard(s);
		return 0;
	}
	s->consec_invalid_headers = 0;
	if (!(bmh & 0x40))
		s->non_err_bytes_since_good += length;
	const uint8_t fid = bmh & 1U;
	const uint32_t pts = pts_present ? ((uint32_t)payload[2] | (uint32_t)payload[3] << 8 |
					    (uint32_t)payload[4] << 16 | (uint32_t)payload[5] << 24)
					 : 0;
	bool changed = s->have_identity && (fid != s->fid || (s->have_pts && pts_present && pts != s->pts));
	if (changed) {
		if (s->active) {
			s->bad_frames++; /* Incomplete frame before the next identity. */
			s->size_errors++;
			s->consec_size_errors++;
		}
		s->bytes = 0;
		s->active = false;
		s->discarding = false;
		s->completed = false;
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
		/* A header-only terminator is valid after a size-complete frame.
		 * Further image bytes or ERR cannot belong to that completed frame. */
		if (s->completed && ((bmh & 0x40) || length > hle)) {
			s->bad_frames++;
			if (bmh & 0x40) {
				s->err_packets++;
			} else {
				s->size_errors++;
				s->consec_size_errors++;
			}
			s->completed = false;
		} else if (bmh & 0x40) {
			s->err_packets++;
		}
		return 0;
	}
	if (bmh & 0x40) {
		s->err_packets++;
		uvc_raw_payload_discard(s);
		return 0;
	}
	s->active = true;
	size_t count = length - hle;
	if (count > s->expected_size - s->bytes) {
		s->size_errors++;
		s->consec_size_errors++;
		uvc_raw_payload_discard(s);
		return 0;
	}
	memcpy(s->frame + s->bytes, payload + hle, count);
	s->bytes += count;
	if (s->bytes != s->expected_size && !(bmh & 0x02)) {
		return 0;
	}
	if (s->bytes != s->expected_size) {
		s->size_errors++;
		s->consec_size_errors++;
		uvc_raw_payload_discard(s);
		return 0;
	}
	s->active = false;
	s->discarding = true; /* Keep identity to reject extra same-frame data. */
	s->completed = true;
	s->good_frames++;
	s->bytes_since_good = 0;
	s->non_err_bytes_since_good = 0;
	s->consec_invalid_headers = 0;
	s->consec_size_errors = 0;
	return 1;
}
/* Returns true if payload-framed Bulk parsing does not match the device's
 * framing and the caller should fall back to byte-stream scanning, both
 * during startup and if sustained desync occurs later at runtime. */
static inline bool uvc_raw_payload_should_fallback(const struct uvc_raw_payload_state *s)
{
	if (!s || !s->expected_size)
		return false;
	const bool established = s->good_frames >= 2U;
	const uint32_t invalid_limit = established ? 8U : 4U;
	const uint32_t size_err_limit = established ? 6U : 3U;
	const uint64_t unit = (uint64_t)s->expected_size;
	const uint64_t size_err_mult = established ? 4U : 3U;
	const uint64_t no_frame_mult = established ? 6U : 4U;
	const uint64_t size_err_bytes = unit > UINT64_MAX / size_err_mult ? UINT64_MAX : unit * size_err_mult;
	const uint64_t no_frame_bytes = unit > UINT64_MAX / no_frame_mult ? UINT64_MAX : unit * no_frame_mult;

	if (s->consec_invalid_headers >= invalid_limit)
		return true;
	if (s->consec_size_errors >= size_err_limit && s->bytes_since_good >= size_err_bytes)
		return true;
	if (s->non_err_bytes_since_good >= no_frame_bytes)
		return true;
	return false;
}
