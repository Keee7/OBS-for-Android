#include <pthread.h>

extern int __real_srt_startup(void);
extern int __real_srt_cleanup(void);

static pthread_mutex_t g_srt_lifecycle_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_srt_lifecycle_refcount = 0;

/*
 * Workaround for libsrt 1.5.4 (srtcore/api.cpp CUDTUnited::startup/cleanup):
 * startup() returns 1 early when m_bGCStatus is true before incrementing
 * m_iInstanceCount, so m_iInstanceCount never exceeds 1. Any concurrent SRT
 * source or output calling srt_cleanup() drops m_iInstanceCount from 1 to 0,
 * sets m_bClosing = true, joins SRT:GC, and garbageCollect() breaks every
 * active SRT socket in the module. Extra cleanups then underflow below 0.
 *
 * Wrapping srt_startup/srt_cleanup with ld --wrap maintains a balanced,
 * thread-safe reference count across both FFmpeg's libavformat/libsrt.c and
 * OBS's obs-ffmpeg-srt.h, calling __real_srt_startup() only on 0 -> 1 and
 * __real_srt_cleanup() only on 1 -> 0.
 */
__attribute__((visibility("hidden"))) int __wrap_srt_startup(void)
{
	pthread_mutex_lock(&g_srt_lifecycle_mutex);
	if (g_srt_lifecycle_refcount > 0) {
		g_srt_lifecycle_refcount++;
		pthread_mutex_unlock(&g_srt_lifecycle_mutex);
		return 1;
	}

	const int ret = __real_srt_startup();
	if (ret >= 0)
		g_srt_lifecycle_refcount = 1;
	pthread_mutex_unlock(&g_srt_lifecycle_mutex);
	return ret;
}

__attribute__((visibility("hidden"))) int __wrap_srt_cleanup(void)
{
	pthread_mutex_lock(&g_srt_lifecycle_mutex);
	if (g_srt_lifecycle_refcount <= 0) {
		pthread_mutex_unlock(&g_srt_lifecycle_mutex);
		return 0;
	}

	if (--g_srt_lifecycle_refcount > 0) {
		pthread_mutex_unlock(&g_srt_lifecycle_mutex);
		return 0;
	}

	const int ret = __real_srt_cleanup();
	pthread_mutex_unlock(&g_srt_lifecycle_mutex);
	return ret;
}
