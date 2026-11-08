/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define UVC_RECOVERY_FRAME_TIMEOUT_NS 8000000000ULL
#define UVC_RECOVERY_RESET_INTERVAL_NS 30000000000ULL

enum uvc_recovery_action { UVC_RECOVERY_WAIT, UVC_RECOVERY_RESTART, UVC_RECOVERY_RESET };
struct uvc_recovery {
	uint64_t since_ns;
	bool restarted;
};
struct uvc_reset_limit {
	uint64_t last_ns;
	bool attempted;
};
static inline void uvc_recovery_start(struct uvc_recovery *r, uint64_t now)
{
	r->since_ns = now;
	r->restarted = false;
}
static inline void uvc_recovery_frame(struct uvc_recovery *r, uint64_t now)
{
	uvc_recovery_start(r, now);
}
static inline void uvc_recovery_restart_finished(struct uvc_recovery *r, uint64_t now)
{
	r->since_ns = now;
	r->restarted = true;
}
static inline enum uvc_recovery_action uvc_recovery_poll(const struct uvc_recovery *r, uint64_t now)
{
	if (now < r->since_ns || now - r->since_ns < UVC_RECOVERY_FRAME_TIMEOUT_NS)
		return UVC_RECOVERY_WAIT;
	return r->restarted ? UVC_RECOVERY_RESET : UVC_RECOVERY_RESTART;
}
static inline bool uvc_recovery_allow_reset(struct uvc_reset_limit *limit, uint64_t now)
{
	if (limit->attempted && (now < limit->last_ns || now - limit->last_ns < UVC_RECOVERY_RESET_INTERVAL_NS))
		return false;
	limit->last_ns = now;
	limit->attempted = true;
	return true;
}
