#include "../android-egl-retry.h"
#include <assert.h>
#include <stdio.h>

struct fake_surface { int calls; bool create_works; };
static bool create_surface(void *opaque)
{
	struct fake_surface *surface = opaque;
	surface->calls++;
	return surface->create_works;
}

int main(void)
{
	struct android_egl_retry retry = {0};
	struct fake_surface surface = {0};
	const uint64_t second = ANDROID_EGL_RETRY_NS;
	android_egl_retry_invalidate(&retry, 10);
	assert(!android_egl_retry_run(&retry, second, create_surface, &surface));
	assert(surface.calls == 0);
	assert(!android_egl_retry_run(&retry, second + 10, create_surface, &surface));
	for (uint64_t t = second + 11; t < second + 1010; ++t)
		assert(!android_egl_retry_run(&retry, t, create_surface, &surface));
	assert(surface.calls == 1);
	/* No new Java Surface event: the same window becomes usable again. */
	surface.create_works = true;
	assert(android_egl_retry_run(&retry, 2 * second + 10, create_surface, &surface));
	/* A failed first swap must not reset the retry deadline. */
	android_egl_retry_invalidate(&retry, 2 * second + 11);
	assert(!android_egl_retry_run(&retry, 3 * second + 9, create_surface, &surface));
	assert(android_egl_retry_run(&retry, 3 * second + 10, create_surface, &surface));
	android_egl_retry_succeeded(&retry);
	assert(!android_egl_retry_run(&retry, 9 * second, create_surface, &surface));
	puts("android EGL retry tests passed");
}
