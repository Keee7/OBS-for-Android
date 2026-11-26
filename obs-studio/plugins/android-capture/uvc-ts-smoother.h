/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Arrival-timestamp smoother for UVC capture when OBS async frame buffering is enabled.
 *
 * Goals:
 *  1. Start from the negotiated frame interval and gently absorb small USB URB
 *     completion jitter (+/- 3-6 ms) via a 1/16 phase-locked step.
 *  2. Preserve real frame drops, transport discontinuities, and session/mode
 *     renegotiations by re-anchoring directly to the post-gap arrival time
 *     instead of smearing one interruption across subsequent frames.
 *  3. Detect sustained same-sign deviation (e.g., actual camera crystal offset
 *     or unannounced rate change) and re-anchor cleanly before large sawtooth
 *     jumps develop.
 *  4. Guarantee strict monotonicity across all transitions, including bypass
 *     (manual low-latency unbuffered mode) and renegotiation.
 */

#define UVC_TS_MIN_PERIOD_NS 4000000ULL   /* 250 fps */
#define UVC_TS_MAX_PERIOD_NS 200000000ULL /* 5 fps */
#define UVC_TS_SUSTAINED_LIMIT 8U

struct uvc_ts_smoother {
	uint64_t period_ns;
	uint64_t smoothed_ns;
	uint64_t last_raw_ns;
	uint64_t last_out_ns;
	bool valid;
	bool discontinuity_pending;
	int sustained_sign;
	uint32_t sustained_count;

	/* Windowed diagnostic counters */
	uint64_t total_frames;
	uint64_t smoothed_frames;
	uint64_t realign_discontinuity;
	uint64_t realign_gap;
	uint64_t realign_drift;
	uint64_t monotonic_clamps;
	int64_t max_abs_err_ns;
};

static inline void uvc_ts_smoother_reset(struct uvc_ts_smoother *sm)
{
	if (!sm)
		return;
	sm->valid = false;
	sm->discontinuity_pending = false;
	sm->sustained_sign = 0;
	sm->sustained_count = 0;
}

static inline void uvc_ts_smoother_configure(struct uvc_ts_smoother *sm, uint32_t frame_interval_100ns)
{
	if (!sm)
		return;
	sm->period_ns = (uint64_t)frame_interval_100ns * 100ULL;
	uvc_ts_smoother_reset(sm);
}

static inline void uvc_ts_smoother_mark_discontinuity(struct uvc_ts_smoother *sm)
{
	if (!sm)
		return;
	sm->discontinuity_pending = true;
}

static inline void uvc_ts_smoother_reset_window(struct uvc_ts_smoother *sm)
{
	if (!sm)
		return;
	sm->total_frames = 0;
	sm->smoothed_frames = 0;
	sm->realign_discontinuity = 0;
	sm->realign_gap = 0;
	sm->realign_drift = 0;
	sm->monotonic_clamps = 0;
	sm->max_abs_err_ns = 0;
}

static inline uint64_t uvc_ts_smoother_step(struct uvc_ts_smoother *sm, uint64_t raw_ts, bool enabled)
{
	if (!sm)
		return raw_ts;

	sm->total_frames++;
	uint64_t out_ts = raw_ts;

	if (enabled && sm->period_ns >= UVC_TS_MIN_PERIOD_NS && sm->period_ns <= UVC_TS_MAX_PERIOD_NS) {
		if (!sm->valid) {
			sm->smoothed_ns = raw_ts;
			sm->valid = true;
			sm->discontinuity_pending = false;
			sm->sustained_sign = 0;
			sm->sustained_count = 0;
		} else if (sm->discontinuity_pending) {
			/* Confirmed drop or transport break: preserve true gap and re-anchor cleanly. */
			sm->smoothed_ns = raw_ts;
			sm->discontinuity_pending = false;
			sm->sustained_sign = 0;
			sm->sustained_count = 0;
			sm->realign_discontinuity++;
		} else {
			const uint64_t pred = sm->smoothed_ns + sm->period_ns;
			const int64_t err = (int64_t)(raw_ts - pred);
			const int64_t snap_limit = (int64_t)((sm->period_ns * 3ULL) / 4ULL);
			const bool backward_raw = raw_ts < sm->last_raw_ns;
			const uint64_t raw_dt = backward_raw ? 0ULL : (raw_ts - sm->last_raw_ns);
			const uint64_t drop_gap_limit = (sm->period_ns * 3ULL) / 2ULL;

			if (backward_raw || raw_dt >= drop_gap_limit || err > snap_limit || err < -snap_limit) {
				/* Real missed frame(s) or arrival discontinuity: preserve true interval. */
				sm->smoothed_ns = raw_ts;
				sm->sustained_sign = 0;
				sm->sustained_count = 0;
				sm->realign_gap++;
			} else {
				const int64_t drift_thresh = (int64_t)(sm->period_ns / 4ULL);
				if (err > drift_thresh || err < -drift_thresh) {
					const int sign = err > 0 ? 1 : -1;
					if (sign == sm->sustained_sign)
						sm->sustained_count++;
					else {
						sm->sustained_sign = sign;
						sm->sustained_count = 1;
					}
				} else {
					sm->sustained_sign = 0;
					sm->sustained_count = 0;
				}

				if (sm->sustained_count >= UVC_TS_SUSTAINED_LIMIT) {
					/* Sustained deviation: re-anchor cleanly before sawtooth oscillation. */
					sm->smoothed_ns = raw_ts;
					sm->sustained_sign = 0;
					sm->sustained_count = 0;
					sm->realign_drift++;
				} else {
					sm->smoothed_ns = (uint64_t)((int64_t)pred + err / 16);
					sm->smoothed_frames++;
					const int64_t abs_err = err < 0 ? -err : err;
					if (abs_err > sm->max_abs_err_ns)
						sm->max_abs_err_ns = abs_err;
				}
			}
		}
		out_ts = sm->smoothed_ns;
	} else {
		uvc_ts_smoother_reset(sm);
	}

	sm->last_raw_ns = raw_ts;
	if (out_ts <= sm->last_out_ns) {
		out_ts = sm->last_out_ns + 1;
		sm->monotonic_clamps++;
	}
	sm->last_out_ns = out_ts;
	return out_ts;
}
