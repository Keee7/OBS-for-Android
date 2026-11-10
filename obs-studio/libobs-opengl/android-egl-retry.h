#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Keep retrying while this swapchain exists, but never more than once per second,
 * including when a newly created surface immediately fails to swap again. */
#define ANDROID_EGL_RETRY_NS UINT64_C(1000000000)

struct android_egl_retry {
	uint64_t last_attempt;
	bool pending;
};

static inline void android_egl_retry_invalidate(struct android_egl_retry *retry, uint64_t now)
{
	if (!retry->pending) {
		retry->last_attempt = now;
		retry->pending = true;
	}
}

static inline bool android_egl_retry_run(struct android_egl_retry *retry, uint64_t now,
					bool (*attempt)(void *), void *opaque)
{
	if (!retry->pending || now - retry->last_attempt < ANDROID_EGL_RETRY_NS)
		return false;
	retry->last_attempt = now;
	return attempt(opaque);
}

static inline void android_egl_retry_succeeded(struct android_egl_retry *retry)
{
	retry->pending = false;
}
