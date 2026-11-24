#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define ANDROID_SURFACE_STALL_TIMEOUT_NS UINT64_C(2000000000)
#define ANDROID_SURFACE_DEADLOCK_TIMEOUT_NS UINT64_C(4000000000)
#define ANDROID_SURFACE_RENDER_GAP_RESET_NS UINT64_C(1000000000)
#define ANDROID_SURFACE_STALL_MIN_MISSES 8u
#define ANDROID_SURFACE_STALL_ACTIVE_MIN_SUBMITS 3u
#define ANDROID_SURFACE_HIDDEN_DRAIN_AFTER_NS UINT64_C(150000000)
#define ANDROID_SURFACE_HIDDEN_UNRENDERED_MIN_SUBMITS 3u

struct android_surface_health {
	uint64_t submitted_seq;
	uint64_t acquired_seq;
	uint64_t first_unacquired_submit_ns;
	uint64_t stall_start_ns;
	uint64_t stall_last_render_ns;
	uint64_t last_render_ns;
	uint32_t stall_misses;
	bool has_acquired_frame;
};

static inline void android_surface_health_reset(struct android_surface_health *h)
{
	memset(h, 0, sizeof(*h));
}

static inline void android_surface_health_on_submit(struct android_surface_health *h, uint64_t now_ns)
{
	if (h->submitted_seq <= h->acquired_seq)
		h->first_unacquired_submit_ns = now_ns;
	h->submitted_seq++;
}

static inline void android_surface_health_on_acquire(struct android_surface_health *h, uint64_t now_ns, bool rendered)
{
	h->acquired_seq = h->submitted_seq;
	h->first_unacquired_submit_ns = 0;
	h->stall_start_ns = 0;
	h->stall_last_render_ns = 0;
	h->stall_misses = 0;
	if (rendered) {
		h->has_acquired_frame = true;
		h->last_render_ns = now_ns;
	}
}

static inline bool android_surface_health_on_no_buffer(struct android_surface_health *h, uint64_t now_ns, bool playing)
{
	h->last_render_ns = now_ns;

	if (h->submitted_seq <= h->acquired_seq || (!playing && h->has_acquired_frame)) {
		h->stall_start_ns = 0;
		h->stall_last_render_ns = 0;
		h->stall_misses = 0;
		return false;
	}

	if (h->stall_start_ns == 0 || now_ns < h->stall_last_render_ns ||
	    (now_ns - h->stall_last_render_ns) > ANDROID_SURFACE_RENDER_GAP_RESET_NS) {
		h->stall_start_ns = now_ns;
		h->stall_last_render_ns = now_ns;
		h->stall_misses = 1;
		return false;
	}

	h->stall_last_render_ns = now_ns;
	h->stall_misses++;
	if (h->stall_misses < ANDROID_SURFACE_STALL_MIN_MISSES)
		return false;

	const uint64_t elapsed_ns = (now_ns >= h->stall_start_ns) ? (now_ns - h->stall_start_ns) : 0;
	const uint64_t pending_submits = h->submitted_seq - h->acquired_seq;
	if (!h->has_acquired_frame || pending_submits >= ANDROID_SURFACE_STALL_ACTIVE_MIN_SUBMITS)
		return elapsed_ns >= ANDROID_SURFACE_STALL_TIMEOUT_NS;

	return elapsed_ns >= ANDROID_SURFACE_DEADLOCK_TIMEOUT_NS;
}

static inline bool android_surface_health_should_drain_hidden(const struct android_surface_health *h, uint64_t now_ns)
{
	if (h->submitted_seq <= h->acquired_seq)
		return false;
	if (h->last_render_ns > 0)
		return now_ns >= h->last_render_ns &&
		       (now_ns - h->last_render_ns) >= ANDROID_SURFACE_HIDDEN_DRAIN_AFTER_NS;
	return (h->submitted_seq - h->acquired_seq) >= ANDROID_SURFACE_HIDDEN_UNRENDERED_MIN_SUBMITS &&
	       h->first_unacquired_submit_ns > 0 && now_ns >= h->first_unacquired_submit_ns &&
	       (now_ns - h->first_unacquired_submit_ns) >= ANDROID_SURFACE_HIDDEN_DRAIN_AFTER_NS;
}
