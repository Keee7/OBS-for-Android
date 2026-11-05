/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stdbool.h>
#include <stdint.h>
struct mc_flow {
	uint64_t submitted, output, pending_since, last_output, last_input;
};
/* Caller serializes input/output counters. A pause with no pending input is idle. */
static inline void mc_flow_submit(struct mc_flow *f, uint64_t now)
{
	if (f->submitted <= f->output)
		f->pending_since = now;
	f->submitted++;
}
static inline void mc_flow_output(struct mc_flow *f, uint64_t now)
{
	f->output++;
	f->last_output = now;
}
static inline bool mc_flow_stalled(const struct mc_flow *f, uint64_t now, uint64_t timeout)
{
	uint64_t progress = f->last_output > f->pending_since ? f->last_output : f->pending_since;
	return f->submitted > f->output && now > progress && now - progress > timeout;
}
static inline bool mc_flow_backlogged(const struct mc_flow *f, uint64_t max_pending)
{
	return f->submitted > f->output && f->submitted - f->output > max_pending;
}
/* A stopped input cadence (pause or scheduling gap) gets a fresh wait window,
 * even if the hardware retained a few pipeline frames. Counts stay bounded. */
static inline void mc_flow_input(struct mc_flow *f, uint64_t now, uint64_t max_gap)
{
	if (f->last_input && now > f->last_input && now - f->last_input > max_gap)
		f->pending_since = now;
	f->last_input = now;
	mc_flow_submit(f, now);
}
