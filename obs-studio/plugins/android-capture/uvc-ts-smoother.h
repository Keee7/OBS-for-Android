/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Arrival-timestamp smoother for UVC capture when OBS async frame buffering is enabled.
 *
 * Goals:
 *  1. Start from the negotiated frame interval and gently absorb USB URB
 *     completion jitter via a critically damped 2nd-order PI loop
 *     (phase gain 1/16, frequency gain 1/1024 -> damping ratio zeta = 1.0).
 *  2. Prioritize confirmed discontinuities (frame drops, parser/sequence gaps,
 *     mode renegotiations) for immediate zero-delay re-anchoring at all frame rates.
 *  3. Separate batch recognition/calibration cap (up to UVC_TS_MAX_BATCH_FRAMES = 8)
 *     from timestamp lead-ahead cap (max_lead_ns, bounded by the buffer budget):
 *     - Causal batch tracking lets multi-frame USB bursts settle toward <= raw_ts
 *       without snapping forward to the batch's first frame or clamping to +1 ns.
 *     - Frames that would exceed max_lead_ns or UVC_TS_MAX_BATCH_FRAMES are flagged
 *       via *out_drop so oversized batches skip excess frames cleanly instead of
 *       compressing consecutive intervals to +1 ns or fabricating far-future timestamps.
 *  4. Disambiguate same-batch frames with device PTS when available:
 *     - Uses PROBE dwClockFrequency when >= 1 kHz.
 *     - Otherwise self-calibrates device PTS clock frequency (Hz) from actual host
 *       elapsed time (dPTS / dt_host) sampled at batch-end frames, gated by actual
 *       frame-sequence delta and negotiated period so mixed 2/3/4-frame batches,
 *       32-bit PTS wraparound, and post-reconnect device changes all calibrate cleanly.
 *  5. Gradually adapt active_period_ns within +/-8% of nominal_period_ns and
 *     guarantee strict monotonicity with O(1) state and zero extra frame buffering.
 */

#define UVC_TS_MIN_PERIOD_NS       4000000ULL    /* 250 fps */
#define UVC_TS_MAX_PERIOD_NS       200000000ULL  /* 5 fps */
#define UVC_TS_JITTER_GUARD_MIN_NS 10000000ULL   /* 10 ms minimum immediate-snap guard */
#define UVC_TS_JITTER_EMA_MAX_NS   20000000ULL   /* cap jitter EMA at 20 ms */
#define UVC_TS_ADAPT_MIN_NUM       92ULL         /* -8% period adaptation floor */
#define UVC_TS_ADAPT_MAX_NUM       108ULL        /* +8% period adaptation ceiling */
#define UVC_TS_SUSTAINED_LIMIT     8U
#define UVC_TS_MAX_BATCH_FRAMES    8U            /* max frames recognized/calibrated as one USB batch */
#define UVC_TS_MIN_LEAD_BUDGET_NS  17000000ULL   /* 17 ms minimum default lead-ahead budget */
#define UVC_TS_MAX_LEAD_BUDGET_NS  100000000ULL  /* 100 ms maximum default lead-ahead budget */
#define UVC_TS_MAX_FUTURE_NS       2000000000ULL /* 2 s anomaly guard matching libobs MAX_TS_VAR */

struct uvc_ts_smoother {
	uint64_t period_ns;        /* nominal negotiated frame period */
	uint64_t active_period_ns; /* slowly adapted effective period */
	uint64_t max_lead_ns;      /* cap on smoothed_ns ahead of raw_ts (buffer budget) */
	uint64_t smoothed_ns;
	uint64_t last_raw_ns;
	uint64_t last_out_ns;
	uint64_t jitter_ema_ns;
	uint64_t frame_seq;      /* monotonic received frame index within session */
	uint64_t last_frame_seq; /* frame_seq of previous frame */

	/* Cross-batch PTS clock frequency estimator (Hz) when dwClockFrequency == 0 */
	uint64_t pts_clock_hz_ema;
	uint64_t pts_cal_raw_ns;  /* previous batch-end host arrival time */
	uint64_t pts_cal_seq;     /* previous batch-end frame_seq */
	uint32_t pts_cal_pts;     /* previous batch-end PTS */
	uint64_t pts_orig_raw_ns; /* multi-batch span origin host arrival time */
	uint64_t pts_orig_seq;    /* multi-batch span origin frame_seq */
	uint32_t pts_orig_pts;    /* multi-batch span origin PTS */
	uint32_t pts_stable_count;
	bool pts_cal_valid;

	uint32_t last_pts;
	bool last_has_pts;
	uint32_t batch_frames; /* frames seen in current USB arrival batch */
	bool valid;
	bool discontinuity_pending;
	int pending_gap_sign;
	int64_t pending_gap_abs_err_ns;
	uint32_t pending_gap_count;
	int sustained_sign;
	uint32_t sustained_count;

	/* Windowed diagnostic counters */
	uint64_t total_frames;
	uint64_t smoothed_frames;
	uint64_t realign_discontinuity;
	uint64_t realign_gap;
	uint64_t realign_drift;
	uint64_t monotonic_clamps;
	uint64_t pts_disambiguated;
	uint64_t lead_budget_drops;
	uint64_t future_rejected;
	int64_t max_abs_err_ns;
};

static inline uint64_t uvc_ts_default_max_lead_ns(uint64_t period_ns)
{
	if (period_ns < UVC_TS_MIN_PERIOD_NS || period_ns > UVC_TS_MAX_PERIOD_NS)
		return 33333333ULL;
	uint64_t budget = period_ns * 2ULL;
	if (budget < UVC_TS_MIN_LEAD_BUDGET_NS)
		budget = UVC_TS_MIN_LEAD_BUDGET_NS;
	if (budget > UVC_TS_MAX_LEAD_BUDGET_NS)
		budget = UVC_TS_MAX_LEAD_BUDGET_NS;
	return budget;
}

static inline void uvc_ts_smoother_reset(struct uvc_ts_smoother *sm)
{
	if (!sm)
		return;
	sm->active_period_ns = sm->period_ns;
	sm->jitter_ema_ns = 0;
	sm->frame_seq = 0;
	sm->last_frame_seq = 0;
	sm->pts_clock_hz_ema = 0;
	sm->pts_cal_raw_ns = 0;
	sm->pts_cal_seq = 0;
	sm->pts_cal_pts = 0;
	sm->pts_orig_raw_ns = 0;
	sm->pts_orig_seq = 0;
	sm->pts_orig_pts = 0;
	sm->pts_stable_count = 0;
	sm->pts_cal_valid = false;
	sm->last_pts = 0;
	sm->last_has_pts = false;
	sm->batch_frames = 0;
	sm->valid = false;
	sm->discontinuity_pending = false;
	sm->pending_gap_sign = 0;
	sm->pending_gap_abs_err_ns = 0;
	sm->pending_gap_count = 0;
	sm->sustained_sign = 0;
	sm->sustained_count = 0;
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
	sm->pts_disambiguated = 0;
	sm->lead_budget_drops = 0;
	sm->future_rejected = 0;
	sm->max_abs_err_ns = 0;
}

static inline void uvc_ts_smoother_set_max_lead_ns(struct uvc_ts_smoother *sm, uint64_t max_lead_ns)
{
	if (!sm)
		return;
	sm->max_lead_ns = max_lead_ns;
}

static inline void uvc_ts_smoother_configure(struct uvc_ts_smoother *sm, uint32_t frame_interval_100ns)
{
	if (!sm)
		return;
	sm->period_ns = (uint64_t)frame_interval_100ns * 100ULL;
	sm->max_lead_ns = uvc_ts_default_max_lead_ns(sm->period_ns);
	uvc_ts_smoother_reset(sm);
	uvc_ts_smoother_reset_window(sm);
}

static inline void uvc_ts_smoother_mark_discontinuity(struct uvc_ts_smoother *sm)
{
	if (!sm)
		return;
	sm->discontinuity_pending = true;
}

static inline uint64_t uvc_ts_clamp_period(uint64_t nominal_ns, int64_t candidate_ns)
{
	uint64_t lo = (nominal_ns * UVC_TS_ADAPT_MIN_NUM) / 100ULL;
	uint64_t hi = (nominal_ns * UVC_TS_ADAPT_MAX_NUM) / 100ULL;
	if (lo < UVC_TS_MIN_PERIOD_NS)
		lo = UVC_TS_MIN_PERIOD_NS;
	if (hi > UVC_TS_MAX_PERIOD_NS)
		hi = UVC_TS_MAX_PERIOD_NS;
	if (candidate_ns < (int64_t)lo)
		return lo;
	if (candidate_ns > (int64_t)hi)
		return hi;
	return (uint64_t)candidate_ns;
}

/* Update self-calibrated device PTS clock frequency (Hz) from consecutive batch-end
 * samples using actual host elapsed time (dPTS / dt_host), gated by actual frame-sequence
 * delta and negotiated period. Batch-end sampling ensures mixed 2/3/4-frame batches keep
 * capture_time ~= arrival_time at both endpoints. */
static inline void uvc_ts_pts_calibrate_batch_end(struct uvc_ts_smoother *sm, uint64_t end_raw_ns, uint32_t end_pts,
						  uint64_t end_seq, uint64_t period_ns)
{
	if (!sm->pts_cal_valid) {
		sm->pts_cal_raw_ns = end_raw_ns;
		sm->pts_cal_pts = end_pts;
		sm->pts_cal_seq = end_seq;
		sm->pts_orig_raw_ns = end_raw_ns;
		sm->pts_orig_pts = end_pts;
		sm->pts_orig_seq = end_seq;
		sm->pts_cal_valid = true;
		return;
	}

	if (end_raw_ns <= sm->pts_cal_raw_ns || end_seq <= sm->pts_cal_seq) {
		sm->pts_cal_valid = false;
		return;
	}

	const uint64_t dseq_step = end_seq - sm->pts_cal_seq;
	const uint64_t dt_step = end_raw_ns - sm->pts_cal_raw_ns;
	const uint32_t dpts_step = end_pts - sm->pts_cal_pts; /* unsigned 32-bit handles wraparound */

	if (dpts_step == 0 || dpts_step >= 0x80000000U) {
		/* Duplicate or backward PTS across batches: invalidate self-calibration. */
		sm->pts_stable_count = 0;
		sm->pts_cal_valid = false;
		return;
	}

	const uint64_t expected_ns = dseq_step * period_ns;
	const bool step_plausible = dseq_step >= 1ULL && dseq_step <= (uint64_t)UVC_TS_MAX_BATCH_FRAMES &&
				    dt_step >= expected_ns / 2ULL && dt_step <= (expected_ns * 3ULL) / 2ULL;
	if (!step_plausible) {
		/* Stall or oversized batch gap: re-anchor span without discarding locked Hz. */
		sm->pts_cal_raw_ns = end_raw_ns;
		sm->pts_cal_pts = end_pts;
		sm->pts_cal_seq = end_seq;
		sm->pts_orig_raw_ns = end_raw_ns;
		sm->pts_orig_pts = end_pts;
		sm->pts_orig_seq = end_seq;
		return;
	}

	/* Prefer the longer multi-batch span (up to ~1 s / 2^30 ticks) to average out
	 * USB completion jitter, falling back to the latest step when needed. */
	uint64_t dt_use = dt_step;
	uint32_t dpts_use = dpts_step;
	if (end_raw_ns > sm->pts_orig_raw_ns && end_seq > sm->pts_orig_seq) {
		const uint64_t dt_orig = end_raw_ns - sm->pts_orig_raw_ns;
		const uint32_t dpts_orig = end_pts - sm->pts_orig_pts;
		if (dt_orig >= dt_step && dt_orig <= 1000000000ULL && dpts_orig > 0 && dpts_orig < 0x40000000U) {
			dt_use = dt_orig;
			dpts_use = dpts_orig;
		}
	}

	const uint64_t est_hz = ((uint64_t)dpts_use * 1000000000ULL + dt_use / 2ULL) / dt_use;
	if (est_hz >= 1000ULL && est_hz <= 2000000000ULL) {
		if (sm->pts_clock_hz_ema == 0) {
			sm->pts_clock_hz_ema = est_hz;
			sm->pts_stable_count = 1U;
		} else {
			const uint64_t ema = sm->pts_clock_hz_ema;
			const bool within_band = est_hz >= (ema * 3ULL) / 4ULL && est_hz <= (ema * 5ULL) / 4ULL;
			if (within_band) {
				const uint64_t w = sm->pts_stable_count >= 4U ? 7ULL : 3ULL;
				sm->pts_clock_hz_ema = (ema * w + est_hz) / (w + 1ULL);
				if (sm->pts_stable_count < 64U)
					sm->pts_stable_count++;
			} else {
				sm->pts_clock_hz_ema = est_hz;
				sm->pts_stable_count = 1U;
				sm->pts_orig_raw_ns = sm->pts_cal_raw_ns;
				sm->pts_orig_pts = sm->pts_cal_pts;
				sm->pts_orig_seq = sm->pts_cal_seq;
			}
		}
	}

	/* Slide multi-batch origin forward once span reaches 500 ms so frequency adapts
	 * smoothly while avoiding 32-bit PTS half-range aliasing on high-rate clocks. */
	if (end_raw_ns > sm->pts_orig_raw_ns && (end_raw_ns - sm->pts_orig_raw_ns) >= 500000000ULL) {
		sm->pts_orig_raw_ns = sm->pts_cal_raw_ns;
		sm->pts_orig_pts = sm->pts_cal_pts;
		sm->pts_orig_seq = sm->pts_cal_seq;
	}

	sm->pts_cal_raw_ns = end_raw_ns;
	sm->pts_cal_pts = end_pts;
	sm->pts_cal_seq = end_seq;
}

static inline uint64_t uvc_ts_smoother_step_ex(struct uvc_ts_smoother *sm, uint64_t raw_ts, bool has_pts,
					       uint32_t pts, uint32_t clock_freq, bool same_batch_hint,
					       bool enabled, bool *out_drop)
{
	if (out_drop)
		*out_drop = false;
	if (!sm)
		return raw_ts;

	sm->total_frames++;
	const uint64_t cur_seq = ++sm->frame_seq;
	uint64_t out_ts = raw_ts;
	bool dropped = false;

	if (enabled && sm->period_ns >= UVC_TS_MIN_PERIOD_NS && sm->period_ns <= UVC_TS_MAX_PERIOD_NS) {
		if (sm->active_period_ns < UVC_TS_MIN_PERIOD_NS || sm->active_period_ns > UVC_TS_MAX_PERIOD_NS)
			sm->active_period_ns = sm->period_ns;

		const uint64_t period = sm->active_period_ns;
		const uint64_t lead_budget =
			sm->max_lead_ns > 0 ? sm->max_lead_ns : uvc_ts_default_max_lead_ns(sm->period_ns);
		const bool same_batch_arrival =
			sm->valid && !sm->discontinuity_pending &&
			(same_batch_hint || raw_ts <= sm->last_raw_ns || (raw_ts - sm->last_raw_ns) < period / 4ULL);

		/* Track USB batch boundaries and calibrate PTS clock frequency at batch ends. */
		if (!sm->valid || sm->discontinuity_pending) {
			sm->pts_cal_valid = false;
			sm->batch_frames = 1U;
		} else if (same_batch_arrival) {
			sm->batch_frames++;
		} else {
			if (clock_freq < 1000U) {
				if (sm->last_has_pts && sm->batch_frames <= UVC_TS_MAX_BATCH_FRAMES)
					uvc_ts_pts_calibrate_batch_end(sm, sm->last_raw_ns, sm->last_pts,
								       sm->last_frame_seq, period);
				else
					sm->pts_cal_valid = false;
			}
			sm->batch_frames = 1U;
		}

		/* Compute trusted inter-frame PTS delta (explicit dwClockFrequency or calibrated Hz). */
		uint64_t pts_dt_ns = 0;
		const uint64_t eff_clock_hz =
			clock_freq >= 1000U ? (uint64_t)clock_freq
					    : (sm->pts_stable_count >= 2U ? sm->pts_clock_hz_ema : 0ULL);
		if (sm->valid && !sm->discontinuity_pending && has_pts && sm->last_has_pts) {
			const uint32_t dpts = pts - sm->last_pts; /* unsigned 32-bit wrap-safe */
			if (dpts > 0 && dpts < 0x80000000U) {
				if (eff_clock_hz >= 1000ULL) {
					const uint64_t cand =
						((uint64_t)dpts * 1000000000ULL + eff_clock_hz / 2ULL) / eff_clock_hz;
					if (cand >= period / 4ULL && cand <= period * 4ULL)
						pts_dt_ns = cand;
				}
			} else if (clock_freq < 1000U) {
				sm->pts_stable_count = 0;
				sm->pts_cal_valid = false;
			}
		}

		if (!sm->valid) {
			sm->smoothed_ns = raw_ts;
			sm->valid = true;
			sm->discontinuity_pending = false;
			sm->pending_gap_sign = 0;
			sm->pending_gap_count = 0;
			sm->sustained_sign = 0;
			sm->sustained_count = 0;
		} else if (sm->discontinuity_pending) {
			/* Confirmed drop, sequence gap, or transport break: re-anchor immediately. */
			sm->smoothed_ns = raw_ts;
			sm->discontinuity_pending = false;
			sm->pending_gap_sign = 0;
			sm->pending_gap_count = 0;
			sm->sustained_sign = 0;
			sm->sustained_count = 0;
			sm->realign_discontinuity++;
		} else {
			uint64_t step_ns = period;
			if (same_batch_arrival && pts_dt_ns > 0) {
				step_ns = pts_dt_ns;
				sm->pts_disambiguated++;
			}

			const uint64_t pred = sm->smoothed_ns + step_ns;
			const int64_t err = (int64_t)(raw_ts - pred);
			const int64_t abs_err = err < 0 ? -err : err;
			const bool large_backward_clock =
				raw_ts < sm->last_raw_ns && (sm->last_raw_ns - raw_ts) > period * 2ULL;

			const int64_t soft_snap = (int64_t)((period * 3ULL) / 4ULL);
			const uint64_t batch_guard_ns = period * (uint64_t)(UVC_TS_MAX_BATCH_FRAMES - 1U);
			uint64_t jitter_guard = (sm->jitter_ema_ns * 5ULL) / 2ULL;
			if (jitter_guard < UVC_TS_JITTER_GUARD_MIN_NS)
				jitter_guard = UVC_TS_JITTER_GUARD_MIN_NS;
			const uint64_t hard_snap_u = batch_guard_ns > jitter_guard ? batch_guard_ns : jitter_guard;
			const int64_t hard_snap =
				soft_snap > (int64_t)hard_snap_u ? soft_snap : (int64_t)hard_snap_u;
			const int64_t shrink_margin = (int64_t)(period / 4ULL);

			if (same_batch_arrival && abs_err > soft_snap) {
				/* Same USB arrival batch outside normal jitter band:
				 *  - err > 0: causal catch-up inside batch (pred < raw_ts); keep exact spacing.
				 *  - err < 0: pred ahead of raw_ts; apply bounded causal pull (<= period/16)
				 *    so steady-state batches settle toward <= raw_ts, and enforce both
				 *    UVC_TS_MAX_BATCH_FRAMES and lead_budget separately. Excess frames are
				 *    skipped instead of clamping consecutive timestamps to +1 ns. */
				if (sm->batch_frames > UVC_TS_MAX_BATCH_FRAMES) {
					dropped = true;
				} else if (err > 0) {
					if (sm->pending_gap_sign == 1 &&
					    abs_err + shrink_margin < sm->pending_gap_abs_err_ns)
						sm->pending_gap_abs_err_ns = abs_err;
					sm->smoothed_ns = pred;
					sm->smoothed_frames++;
					if (abs_err > sm->max_abs_err_ns)
						sm->max_abs_err_ns = abs_err;
				} else {
					uint64_t pull = (uint64_t)(-err) / 16ULL;
					if (pull > period / 16ULL)
						pull = period / 16ULL;
					const uint64_t cand = pred - pull;
					if (cand > raw_ts + lead_budget) {
						dropped = true;
					} else {
						sm->smoothed_ns = cand;
						sm->smoothed_frames++;
						if (abs_err > sm->max_abs_err_ns)
							sm->max_abs_err_ns = abs_err;
					}
				}
			} else if (large_backward_clock || (err > 0 && abs_err > hard_snap)) {
				/* Unmistakable forward stall (> batch guard) or backward clock jump. */
				sm->smoothed_ns = raw_ts;
				sm->pending_gap_sign = 0;
				sm->pending_gap_count = 0;
				sm->sustained_sign = 0;
				sm->sustained_count = 0;
				sm->realign_gap++;
			} else if (abs_err > soft_snap) {
				/* Cross-batch guard band:
				 *  - Positive error (soft_snap < err <= hard_snap): either first frame of a
				 *    causal multi-frame batch (shrinking on subsequent frames) or a real
				 *    1..7-frame silent drop (confirmed on next non-shrinking frame).
				 *  - Negative error across distinct batches: confirm over 3 batches while
				 *    respecting lead_budget so we never emit far-future timestamps. */
				const int sign = err > 0 ? 1 : -1;
				const uint32_t confirm_needed = sign < 0 ? 3U : 2U;
				if (sign == sm->pending_gap_sign) {
					if (abs_err + shrink_margin >= sm->pending_gap_abs_err_ns) {
						sm->pending_gap_count++;
					} else {
						sm->pending_gap_abs_err_ns = abs_err;
					}
				} else {
					sm->pending_gap_sign = sign;
					sm->pending_gap_abs_err_ns = abs_err;
					sm->pending_gap_count = 1U;
				}

				if (sm->pending_gap_count >= confirm_needed) {
					sm->smoothed_ns = raw_ts;
					sm->pending_gap_sign = 0;
					sm->pending_gap_abs_err_ns = 0;
					sm->pending_gap_count = 0;
					sm->sustained_sign = 0;
					sm->sustained_count = 0;
					sm->realign_gap++;
				} else if (sign > 0) {
					sm->smoothed_ns = pred;
					sm->smoothed_frames++;
					if (abs_err > sm->max_abs_err_ns)
						sm->max_abs_err_ns = abs_err;
				} else {
					uint64_t pull = (uint64_t)(-err) / 16ULL;
					if (pull > period / 16ULL)
						pull = period / 16ULL;
					const uint64_t cand = pred - pull;
					if (cand > raw_ts + lead_budget) {
						dropped = true;
					} else {
						sm->smoothed_ns = cand;
						sm->smoothed_frames++;
						if (abs_err > sm->max_abs_err_ns)
							sm->max_abs_err_ns = abs_err;
					}
				}
			} else {
				/* Normal jitter band: clear pending gap and update recent jitter envelope. */
				sm->pending_gap_sign = 0;
				sm->pending_gap_count = 0;

				uint64_t sample_jit = (uint64_t)abs_err;
				if (sample_jit > UVC_TS_JITTER_EMA_MAX_NS)
					sample_jit = UVC_TS_JITTER_EMA_MAX_NS;
				if (sm->jitter_ema_ns == 0)
					sm->jitter_ema_ns = sample_jit;
				else
					sm->jitter_ema_ns = (sm->jitter_ema_ns * 7ULL + sample_jit) / 8ULL;

				/* Gently adapt effective period (critically damped: Kp=1/16, Ki=1/1024). */
				const uint64_t next_period =
					uvc_ts_clamp_period(sm->period_ns, (int64_t)period + err / 1024);
				const bool period_clamped = (next_period == period) && (err / 1024 != 0);
				sm->active_period_ns = next_period;

				const int64_t drift_thresh = (int64_t)(next_period / 4ULL);
				if (period_clamped && abs_err > drift_thresh) {
					const int sign = err > 0 ? 1 : -1;
					if (sign == sm->sustained_sign)
						sm->sustained_count++;
					else {
						sm->sustained_sign = sign;
						sm->sustained_count = 1U;
					}
				} else {
					sm->sustained_sign = 0;
					sm->sustained_count = 0;
				}

				if (sm->sustained_count >= UVC_TS_SUSTAINED_LIMIT) {
					/* Device deviated outside +/-8% band (e.g. unannounced 60->30 AE drop). */
					sm->smoothed_ns = raw_ts;
					sm->sustained_sign = 0;
					sm->sustained_count = 0;
					sm->realign_drift++;
				} else {
					sm->smoothed_ns = (uint64_t)((int64_t)pred + err / 16);
					sm->smoothed_frames++;
					if (abs_err > sm->max_abs_err_ns)
						sm->max_abs_err_ns = abs_err;
				}
			}
		}

		if (dropped) {
			sm->lead_budget_drops++;
			if (out_drop) {
				*out_drop = true;
				/* Update arrival/PTS/seq trackers so batch-end calibration stays exact,
				 * without advancing smoothed_ns or emitting a clamped timestamp. */
				sm->last_raw_ns = raw_ts;
				sm->last_frame_seq = cur_seq;
				sm->last_has_pts = has_pts;
				if (has_pts)
					sm->last_pts = pts;
				return 0;
			}
			/* Legacy callers without out_drop: cap output to lead_budget. */
			sm->smoothed_ns = raw_ts + lead_budget;
		}
		out_ts = sm->smoothed_ns;
	} else {
		uvc_ts_smoother_reset(sm);
	}

	sm->last_raw_ns = raw_ts;
	sm->last_frame_seq = cur_seq;
	sm->last_has_pts = has_pts;
	if (has_pts)
		sm->last_pts = pts;
	if (out_ts <= sm->last_out_ns) {
		out_ts = sm->last_out_ns + 1;
		sm->monotonic_clamps++;
	}
	sm->last_out_ns = out_ts;
	return out_ts;
}

static inline uint64_t uvc_ts_smoother_step(struct uvc_ts_smoother *sm, uint64_t raw_ts, bool enabled)
{
	return uvc_ts_smoother_step_ex(sm, raw_ts, false, 0, 0, false, enabled, NULL);
}
