/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "ndi-runtime.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
typedef HMODULE ndi_lib_handle_t;
static ndi_lib_handle_t ndi_dlopen(const char *path)
{
	return LoadLibraryA(path);
}
static void *ndi_dlsym(ndi_lib_handle_t h, const char *sym)
{
	return (void *)GetProcAddress(h, sym);
}
static void ndi_dlclose(ndi_lib_handle_t h)
{
	if (h)
		FreeLibrary(h);
}
static const char *ndi_dlerror(void)
{
	return "LoadLibrary failed";
}
#else
#include <dlfcn.h>
typedef void *ndi_lib_handle_t;
static ndi_lib_handle_t ndi_dlopen(const char *path)
{
	return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}
static void *ndi_dlsym(ndi_lib_handle_t h, const char *sym)
{
	return dlsym(h, sym);
}
static void ndi_dlclose(ndi_lib_handle_t h)
{
	if (h)
		dlclose(h);
}
static const char *ndi_dlerror(void)
{
	const char *e = dlerror();
	return e ? e : "unknown dlopen error";
}
#endif

#if defined(__ANDROID__)
#include <android/log.h>
#include <obs-android.h>
#endif

static pthread_mutex_t g_ndi_mtx = PTHREAD_MUTEX_INITIALIZER;
static ndi_lib_handle_t g_ndi_lib = NULL;
static struct ndi_api g_ndi_api;
static int g_ndi_refs = 0;
static bool g_ndi_initialized = false;
static char g_ndi_version[128] = {0};
static char g_ndi_path[256] = {0};
static char g_ndi_error[256] = {0};

void ndi_runtime_default_dropin_path(char *out, size_t size)
{
	if (!out || !size)
		return;
	const char *root = getenv("OBS_ROOT_PATH");
	if (root && root[0])
		snprintf(out, size, "%s/ndi/libndi.so", root);
	else
		out[0] = '\0';
}

static bool resolve_required_symbols(ndi_lib_handle_t h, struct ndi_api *api, char *err, size_t err_size)
{
	memset(api, 0, sizeof(*api));
#define REQ_SYM(field, sym)                                                                            \
	do {                                                                                           \
		void *p = ndi_dlsym(h, sym);                                                           \
		if (!p) {                                                                              \
			snprintf(err, err_size, "missing symbol %s", sym);                             \
			return false;                                                                  \
		}                                                                                      \
		memcpy(&api->field, &p, sizeof(p));                                                    \
	} while (0)
#define OPT_SYM(field, sym)                                                                            \
	do {                                                                                           \
		void *p = ndi_dlsym(h, sym);                                                           \
		if (p)                                                                                 \
			memcpy(&api->field, &p, sizeof(p));                                            \
	} while (0)

	REQ_SYM(initialize, "NDIlib_initialize");
	REQ_SYM(destroy, "NDIlib_destroy");
	REQ_SYM(version, "NDIlib_version");

	REQ_SYM(find_create_v2, "NDIlib_find_create_v2");
	REQ_SYM(find_destroy, "NDIlib_find_destroy");
	REQ_SYM(find_get_current_sources, "NDIlib_find_get_current_sources");
	REQ_SYM(find_wait_for_sources, "NDIlib_find_wait_for_sources");

	REQ_SYM(recv_create_v3, "NDIlib_recv_create_v3");
	REQ_SYM(recv_destroy, "NDIlib_recv_destroy");
	REQ_SYM(recv_connect, "NDIlib_recv_connect");
	REQ_SYM(recv_capture_v3, "NDIlib_recv_capture_v3");
	REQ_SYM(recv_free_video_v2, "NDIlib_recv_free_video_v2");
	REQ_SYM(recv_free_audio_v3, "NDIlib_recv_free_audio_v3");
	REQ_SYM(recv_free_metadata, "NDIlib_recv_free_metadata");

	OPT_SYM(send_create, "NDIlib_send_create");
	OPT_SYM(send_destroy, "NDIlib_send_destroy");
	OPT_SYM(send_send_video_async_v2, "NDIlib_send_send_video_async_v2");
	OPT_SYM(send_send_audio_v3, "NDIlib_send_send_audio_v3");
	OPT_SYM(send_get_no_connections, "NDIlib_send_get_no_connections");

#undef REQ_SYM
#undef OPT_SYM
	return true;
}

static bool ndi_is_supported_version(const char *version)
{
	if (!version)
		return false;
	const char *p = version;
	while (*p) {
		if (*p >= '0' && *p <= '9') {
			char *end = NULL;
			long major = strtol(p, &end, 10);
			if (end != p && (*end == '.' || *end == '\0'))
				return major == 5 || major == 6;
			p = end;
		} else {
			p++;
		}
	}
	return false;
}

static bool try_open_candidate(const char *candidate)
{
	if (!candidate || !candidate[0])
		return false;
	ndi_lib_handle_t h = ndi_dlopen(candidate);
	if (!h) {
		snprintf(g_ndi_error, sizeof(g_ndi_error), "%s: %s", candidate, ndi_dlerror());
		return false;
	}
	struct ndi_api api;
	char sym_err[128] = {0};
	if (!resolve_required_symbols(h, &api, sym_err, sizeof(sym_err))) {
		snprintf(g_ndi_error, sizeof(g_ndi_error), "%s: %s", candidate, sym_err);
		ndi_dlclose(h);
		return false;
	}
	/* NDIlib_version 已是必需符号：初始化前先解析主版本，只接受 5 或 6。 */
	const char *runtime_version = api.version();
	if (!ndi_is_supported_version(runtime_version)) {
		snprintf(g_ndi_error, sizeof(g_ndi_error), "%s: unsupported NDI runtime version \"%s\" (need major 5 or 6)",
			 candidate, runtime_version ? runtime_version : "unknown");
		ndi_dlclose(h);
		return false;
	}
	if (!api.initialize()) {
		snprintf(g_ndi_error, sizeof(g_ndi_error), "%s: NDIlib_initialize() returned false", candidate);
		ndi_dlclose(h);
		return false;
	}
	g_ndi_lib = h;
	g_ndi_api = api;
	g_ndi_initialized = true;
	snprintf(g_ndi_path, sizeof(g_ndi_path), "%s", candidate);
	snprintf(g_ndi_version, sizeof(g_ndi_version), "%s", runtime_version);
	g_ndi_error[0] = '\0';
#if defined(__ANDROID__)
	__android_log_print(ANDROID_LOG_INFO, "OBS-ndi", "Loaded NDI runtime: %s (%s) send=%d", g_ndi_path,
			    g_ndi_version, (api.send_create && api.send_send_video_async_v2) ? 1 : 0);
#endif
	return true;
}

const struct ndi_api *ndi_runtime_acquire(void)
{
	pthread_mutex_lock(&g_ndi_mtx);
	if (g_ndi_initialized) {
		g_ndi_refs++;
		pthread_mutex_unlock(&g_ndi_mtx);
		return &g_ndi_api;
	}

	/* 只接受应用私有目录里的运行库：$OBS_ROOT_PATH/ndi 与 Java 侧给出的
	 * native lib dir。不再读环境变量、不再回退系统搜索路径。 */
	char buf[384];
	const char *root = getenv("OBS_ROOT_PATH");
	if (root && root[0]) {
		snprintf(buf, sizeof(buf), "%s/ndi/libndi.so", root);
		if (try_open_candidate(buf)) {
			g_ndi_refs = 1;
			pthread_mutex_unlock(&g_ndi_mtx);
			return &g_ndi_api;
		}
	}

#if defined(__ANDROID__)
	char nat_dir[256] = {0};
	if (obs_android_ndi_native_lib_dir(nat_dir, sizeof(nat_dir))) {
		snprintf(buf, sizeof(buf), "%s/libndi.so", nat_dir);
		if (try_open_candidate(buf)) {
			g_ndi_refs = 1;
			pthread_mutex_unlock(&g_ndi_mtx);
			return &g_ndi_api;
		}
	}
#endif

	pthread_mutex_unlock(&g_ndi_mtx);
	return NULL;
}

void ndi_runtime_release(void)
{
	pthread_mutex_lock(&g_ndi_mtx);
	if (g_ndi_refs > 0)
		g_ndi_refs--;
	if (g_ndi_refs == 0 && g_ndi_initialized) {
		if (g_ndi_api.destroy)
			g_ndi_api.destroy();
		g_ndi_initialized = false;
		memset(&g_ndi_api, 0, sizeof(g_ndi_api));
		if (g_ndi_lib) {
			ndi_dlclose(g_ndi_lib);
			g_ndi_lib = NULL;
		}
	}
	pthread_mutex_unlock(&g_ndi_mtx);
}

bool ndi_runtime_is_loaded(void)
{
	pthread_mutex_lock(&g_ndi_mtx);
	const bool ok = g_ndi_initialized;
	pthread_mutex_unlock(&g_ndi_mtx);
	return ok;
}

void ndi_runtime_get_info(char *version_out, size_t version_size, char *path_out, size_t path_size, char *error_out,
			  size_t error_size)
{
	pthread_mutex_lock(&g_ndi_mtx);
	if (version_out && version_size)
		snprintf(version_out, version_size, "%s", g_ndi_version);
	if (path_out && path_size)
		snprintf(path_out, path_size, "%s", g_ndi_path);
	if (error_out && error_size)
		snprintf(error_out, error_size, "%s", g_ndi_error);
	pthread_mutex_unlock(&g_ndi_mtx);
}
