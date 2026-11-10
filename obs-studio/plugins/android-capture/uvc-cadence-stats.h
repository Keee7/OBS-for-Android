/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* A sparse 64x64 luma fingerprint is diagnostic only: equal samples do not
 * prove equal frames, and can miss motion between sample locations. */
struct uvc_cadence_stats {
	uint64_t samples, same_hash, dt[5], max_dt;
	uint64_t previous_ns, previous_hash;
	uint64_t pts_frames, pts_pairs, pts_same, pts_wraps;
	uint32_t previous_pts, pts_delta_min, pts_delta_max;
	bool have_previous_pts;
	bool have_previous;
};
static inline void uvc_cadence_observe(struct uvc_cadence_stats *s, const uint8_t *data, unsigned width,
				       unsigned height, bool yuyv, uint64_t received_ns)
{
	uint64_t hash = 1469598103934665603ULL;
	for (unsigned y = 0; y < 64; y++) {
		size_t row = (size_t)((uint64_t)y * height / 64) * width * (yuyv ? 2U : 1U);
		for (unsigned x = 0; x < 64; x++) {
			size_t col = (size_t)((uint64_t)x * width / 64) * (yuyv ? 2U : 1U);
			hash = (hash ^ data[row + col]) * 1099511628211ULL;
		}
	}
	s->samples++;
	if (s->have_previous) {
		if (hash == s->previous_hash) {
			s->same_hash++;
		}
		if (received_ns >= s->previous_ns) {
			uint64_t d = received_ns - s->previous_ns;
			unsigned bucket = d < 8000000ULL    ? 0
					  : d < 14000000ULL ? 1
					  : d < 19000000ULL ? 2
					  : d < 25000000ULL ? 3
							    : 4;
			s->dt[bucket]++;
			if (d > s->max_dt) {
				s->max_dt = d;
			}
		}
	}
	s->previous_ns = received_ns;
	s->previous_hash = hash;
	s->have_previous = true;
}
static inline void uvc_cadence_observe_pts(struct uvc_cadence_stats *s, bool present, uint32_t pts)
{
	if (!present) {
		s->have_previous_pts = false;
		return;
	}
	s->pts_frames++;
	if (s->have_previous_pts) {
		uint32_t delta = pts - s->previous_pts; /* modular; wraps or resets need separate interpretation */
		if (!s->pts_pairs || delta < s->pts_delta_min) {
			s->pts_delta_min = delta;
		}
		if (!s->pts_pairs || delta > s->pts_delta_max) {
			s->pts_delta_max = delta;
		}
		s->pts_pairs++;
		s->pts_same += delta == 0;
		s->pts_wraps += pts < s->previous_pts;
	}
	s->previous_pts = pts;
	s->have_previous_pts = true;
}
static inline void uvc_cadence_reset_window(struct uvc_cadence_stats *s)
{
	s->samples = s->same_hash = s->max_dt = 0;
	s->pts_frames = s->pts_pairs = s->pts_same = s->pts_wraps = 0;
	s->pts_delta_min = s->pts_delta_max = 0;
	memset(s->dt, 0, sizeof(s->dt));
}
