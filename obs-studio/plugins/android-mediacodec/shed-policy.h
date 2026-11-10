/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* High-stability load-shedding policy for the Android MediaCodec encoders.
 *
 * Lives in its own header so the host regression test exercises the real
 * decision logic rather than a hand-copied mirror of it.
 *
 * Why the soft limit is an ABSOLUTE depth and not a fraction of the hard
 * limit: it used to be max_pending - 100, which at 60fps meant 200 frames of
 * backlog. On the 25053RP5CC (c2.xring, HEVC and H.264 alike) the pending
 * count sat at 0-2 frames for a whole minute and then jumped straight past 200
 * to the hard limit, so the soft limit never fired in the only case it existed
 * for. 10 frames is ~165ms at 60fps: enough slack for scheduler jitter and
 * codec bursts, shallow enough that a degrading codec is noticed within a few
 * frames. */
#define MC_SHED_SOFT_PENDING 10

/* Degraded depth: once submits are measured over budget the codec is
 * saturated, so only this much backlog is tolerated before shedding. */
#define MC_SHED_MIN_PENDING 4

/* `degraded` saturates here. >= 2 collapses the soft limit to the floor;
 * >= 3 additionally sheds while any frame is still in flight. */
#define MC_SHED_DEGRADED_MAX 3

/* Retry window while shedding: long enough for the codec to drain a burst,
 * short enough that quality recovers quickly once it does. */
#define MC_SHED_RETRY_NS 500000000ULL

/* Age the degraded state: `degraded` is 0 when healthy, rises when a submit
 * exceeds the budget and falls when it is comfortably below. While shedding we
 * never submit, so the only way out of the collapse is this time decay. */
static inline void mc_shed_tick(int *degraded, uint64_t *until_ns, uint64_t now)
{
	if (*degraded && *until_ns && now >= *until_ns) {
		/* Step down to 1, not 0: the next submit still has to prove the codec
		 * recovered. Clearing to 0 would re-admit a full soft-limit burst on
		 * every window even while the codec stays saturated. One fast submit
		 * then decays 1 -> 0 through mc_shed_note_submit(). */
		*degraded = 1;
		*until_ns = 0;
	}
}

static inline uint64_t mc_shed_soft_limit(int degraded)
{
	return degraded >= 2 ? MC_SHED_MIN_PENDING : MC_SHED_SOFT_PENDING;
}

/* Two independent arms, because the device showed both regimes. Backlog arm:
 * pending is climbing, the codec is not draining. Degraded arm: every submit
 * measured over budget while pending stayed near zero -- the blocking happened
 * inside eglSwapBuffers, not in the codec input queue. That was the whole 60
 * seconds before the 25053RP5CC tore the stream down, and it is the regime a
 * pending-only gate can never catch. Requiring degraded >= 3 keeps one
 * scheduler hiccup from collapsing the frame rate. */
static inline bool mc_shed_decide(uint64_t pending, int degraded, uint64_t soft_limit)
{
	return pending >= soft_limit || (degraded >= 3 && pending > 0);
}

/* Record one real submit. Returns true when it was over budget. */
static inline bool mc_shed_note_submit(uint64_t submit_us, uint64_t budget_us, int *degraded,
				       uint64_t *until_ns, uint64_t now)
{
	if (submit_us > budget_us) {
		*degraded = *degraded < MC_SHED_DEGRADED_MAX ? *degraded + 1 : MC_SHED_DEGRADED_MAX;
		*until_ns = now + MC_SHED_RETRY_NS;
		return true;
	}
	if (*degraded && submit_us * 2 < budget_us) {
		/* Recovered: step back down and clear the retry deadline, so a later
		 * degradation starts a fresh window instead of inheriting a stale one
		 * that may already be in the past. */
		if (--*degraded == 0)
			*until_ns = 0;
	}
	return false;
}
