/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

/*
 * Shared NDI A/V clock and cadence tracker.
 *
 * Uses a single consistent time domain per receive session:
 *   - Prefers source timecode (100 ns) so sender-side encoding/queueing latency
 *     between video and audio submission timestamps does NOT skew A/V sync.
 *   - Verifies that video and audio timecodes belong to the same epoch
 *     (within NDI_SYNC_TC_COHERENCE_100NS). If either stream lacks a valid
 *     timecode or their timecodes diverge while their SDK timestamps agree,
 *     the entire session cleanly switches to the SDK timestamp domain once.
 *   - Maps onto OBS's monotonic nanosecond timeline with discontinuity rebase
 *     and gentle crystal-drift absorption.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define NDI_SYNC_JUMP_FORWARD_NS 500000000LL     /* 500 ms */
#define NDI_SYNC_JUMP_BACKWARD_NS 200000000LL    /* 200 ms */
#define NDI_SYNC_DRIFT_LIMIT_NS 150000000LL      /* 150 ms */
#define NDI_SYNC_TC_COHERENCE_100NS 100000000LL  /* 10 s */
#define NDI_SYNC_TS_COHERENCE_100NS 20000000LL   /* 2 s */

enum ndi_sync_mode {
	NDI_SYNC_MODE_SOURCE_TIMING = 0, /* Auto: prefer coherent source timecode, fallback to SDK timestamp */
	NDI_SYNC_MODE_INTERNAL = 1,      /* Local OBS monotonic receive clock (ignore sender timestamps) */
	NDI_SYNC_MODE_NETWORK = 2,       /* Force NDI SDK submission timestamp */
};

enum ndi_sync_domain {
	NDI_SYNC_DOMAIN_NONE = 0,
	NDI_SYNC_DOMAIN_TIMECODE = 1,
	NDI_SYNC_DOMAIN_TIMESTAMP = 2,
};

struct ndi_sync_clock {
	bool anchored;
	enum ndi_sync_domain domain;
	int64_t base_ndi_100ns;
	uint64_t base_obs_ns;
	int64_t last_video_tc_100ns;
	int64_t last_audio_tc_100ns;
	int64_t last_video_ts_100ns;
	int64_t last_audio_ts_100ns;
	uint64_t last_video_ts;
	uint64_t last_audio_ts;
	uint64_t last_video_hash;
	bool have_video_hash;

	uint64_t video_frames;
	uint64_t unique_video_frames;
	uint64_t duplicate_video_frames;
	uint64_t audio_frames;
	uint64_t discontinuities;
	int64_t last_av_offset_ms;
};

static inline void ndi_sync_reset(struct ndi_sync_clock *c)
{
	if (!c)
		return;
	memset(c, 0, sizeof(*c));
}

static inline bool ndi_sync_valid_100ns(int64_t v)
{
	return v != INT64_MAX && v > 0;
}

static inline int64_t ndi_sync_abs64(int64_t v)
{
	return v < 0 ? -v : v;
}

static inline int64_t ndi_sync_pick_100ns(int64_t timestamp, int64_t timecode)
{
	if (ndi_sync_valid_100ns(timecode))
		return timecode;
	if (ndi_sync_valid_100ns(timestamp))
		return timestamp;
	return 0;
}

static inline uint64_t ndi_sync_map_ts(struct ndi_sync_clock *c, int64_t ndi_100ns, uint64_t now_obs_ns, bool is_video)
{
	if (!c)
		return now_obs_ns;

	uint64_t mapped = now_obs_ns;
	if (ndi_100ns > 0) {
		if (!c->anchored) {
			c->anchored = true;
			c->base_ndi_100ns = ndi_100ns;
			c->base_obs_ns = now_obs_ns;
			mapped = now_obs_ns;
		} else {
			const int64_t delta_100ns = ndi_100ns - c->base_ndi_100ns;
			/* Guard against 64-bit overflow and huge sender discontinuities. */
			if (delta_100ns < -(NDI_SYNC_JUMP_BACKWARD_NS / 100LL) ||
			    delta_100ns > (INT64_MAX / 100LL) - 1000LL) {
				c->base_ndi_100ns = ndi_100ns;
				c->base_obs_ns = now_obs_ns;
				c->discontinuities++;
				mapped = now_obs_ns;
			} else {
				const int64_t candidate_signed = (int64_t)c->base_obs_ns + delta_100ns * 100LL;
				const int64_t drift = candidate_signed - (int64_t)now_obs_ns;
				if (drift > NDI_SYNC_JUMP_FORWARD_NS || drift < -NDI_SYNC_JUMP_FORWARD_NS) {
					c->base_ndi_100ns = ndi_100ns;
					c->base_obs_ns = now_obs_ns;
					c->discontinuities++;
					mapped = now_obs_ns;
				} else if (drift > NDI_SYNC_DRIFT_LIMIT_NS || drift < -NDI_SYNC_DRIFT_LIMIT_NS) {
					/* Gently pull anchor 1/16th toward wall clock to absorb crystal drift. */
					c->base_obs_ns = (uint64_t)((int64_t)c->base_obs_ns - drift / 16LL);
					mapped = (uint64_t)((int64_t)c->base_obs_ns + delta_100ns * 100LL);
				} else {
					mapped = candidate_signed > 0 ? (uint64_t)candidate_signed : now_obs_ns;
				}
			}
		}
	}

	if (is_video) {
		if (mapped <= c->last_video_ts)
			mapped = c->last_video_ts + 1;
		c->last_video_ts = mapped;
		c->video_frames++;
	} else {
		if (mapped <= c->last_audio_ts)
			mapped = c->last_audio_ts + 1;
		c->last_audio_ts = mapped;
		c->audio_frames++;
	}

	if (c->last_video_ts > 0 && c->last_audio_ts > 0) {
		const int64_t diff_ns = (int64_t)c->last_audio_ts - (int64_t)c->last_video_ts;
		c->last_av_offset_ms = diff_ns / 1000000LL;
	}
	return mapped;
}

/*
 * Session-coherent timestamp mapping: selects ONE clock domain (timecode vs
 * timestamp) for both audio and video in this receive session.
 */
static inline uint64_t ndi_sync_map_frame(struct ndi_sync_clock *c, int64_t timestamp, int64_t timecode,
					  uint64_t now_obs_ns, bool is_video)
{
	if (!c)
		return now_obs_ns;

	const bool tc_ok = ndi_sync_valid_100ns(timecode);
	const bool ts_ok = ndi_sync_valid_100ns(timestamp);

	if (is_video) {
		if (tc_ok)
			c->last_video_tc_100ns = timecode;
		if (ts_ok)
			c->last_video_ts_100ns = timestamp;
	} else {
		if (tc_ok)
			c->last_audio_tc_100ns = timecode;
		if (ts_ok)
			c->last_audio_ts_100ns = timestamp;
	}

	if (c->domain == NDI_SYNC_DOMAIN_NONE) {
		if (tc_ok)
			c->domain = NDI_SYNC_DOMAIN_TIMECODE;
		else if (ts_ok)
			c->domain = NDI_SYNC_DOMAIN_TIMESTAMP;
	} else if (c->domain == NDI_SYNC_DOMAIN_TIMECODE) {
		/* Verify cross-stream timecode coherence; switch session to timestamp if streams use different epochs. */
		bool tc_incoherent = false;
		if (!tc_ok && ts_ok) {
			tc_incoherent = true;
		} else if (tc_ok && c->last_video_tc_100ns > 0 && c->last_audio_tc_100ns > 0) {
			const int64_t tc_gap = ndi_sync_abs64(c->last_video_tc_100ns - c->last_audio_tc_100ns);
			if (tc_gap > NDI_SYNC_TC_COHERENCE_100NS && c->last_video_ts_100ns > 0 &&
			    c->last_audio_ts_100ns > 0 &&
			    ndi_sync_abs64(c->last_video_ts_100ns - c->last_audio_ts_100ns) <=
				    NDI_SYNC_TS_COHERENCE_100NS) {
				tc_incoherent = true;
			}
		}
		if (tc_incoherent && ts_ok) {
			c->domain = NDI_SYNC_DOMAIN_TIMESTAMP;
			c->anchored = false;
			c->discontinuities++;
		}
	}

	int64_t chosen = 0;
	if (c->domain == NDI_SYNC_DOMAIN_TIMECODE)
		chosen = tc_ok ? timecode : 0;
	else if (c->domain == NDI_SYNC_DOMAIN_TIMESTAMP)
		chosen = ts_ok ? timestamp : 0;

	return ndi_sync_map_ts(c, chosen, now_obs_ns, is_video);
}

static inline uint64_t ndi_sync_map_frame_mode(struct ndi_sync_clock *c, enum ndi_sync_mode mode, int64_t timestamp,
					       int64_t timecode, uint64_t now_obs_ns, bool is_video)
{
	if (mode == NDI_SYNC_MODE_INTERNAL)
		return ndi_sync_map_ts(c, 0, now_obs_ns, is_video);
	if (mode == NDI_SYNC_MODE_NETWORK) {
		const int64_t ts = ndi_sync_valid_100ns(timestamp) ? timestamp : 0;
		return ndi_sync_map_ts(c, ts, now_obs_ns, is_video);
	}
	return ndi_sync_map_frame(c, timestamp, timecode, now_obs_ns, is_video);
}

/* Fast sparse 64-sample FNV-1a fingerprint to track unique vs duplicate video frames. */
static inline void ndi_sync_observe_video_payload(struct ndi_sync_clock *c, const uint8_t *data, size_t bytes)
{
	if (!c || !data || !bytes)
		return;
	uint64_t hash = 1469598103934665603ULL;
	const size_t step = bytes >= 64 ? bytes / 64 : 1;
	for (size_t i = 0; i < bytes; i += step) {
		hash ^= data[i];
		hash *= 1099511628211ULL;
	}
	if (c->have_video_hash && hash == c->last_video_hash)
		c->duplicate_video_frames++;
	else
		c->unique_video_frames++;
	c->last_video_hash = hash;
	c->have_video_hash = true;
}
