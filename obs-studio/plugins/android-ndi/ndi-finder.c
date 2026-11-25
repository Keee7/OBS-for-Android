/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "ndi-finder.h"
#include "ndi-runtime.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__ANDROID__)
#include <obs-android.h>
#endif

static pthread_mutex_t g_finder_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_finder_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_finder_state_cond = PTHREAD_COND_INITIALIZER;
static pthread_t g_finder_thread;
static bool g_finder_running = false;
static bool g_finder_joining = false;
static bool g_finder_stop = false;
static bool g_finder_recreate = false;
static int g_finder_refs = 0;

#define NDI_FINDER_MAX_OWNERS 32

struct ndi_extra_ip_entry {
	bool in_use;
	const void *owner;
	char ips[256];
};

static struct ndi_extra_ip_entry g_owner_ips[NDI_FINDER_MAX_OWNERS];
static char g_user_extra_ips[1024] = {0};
static struct ndi_discovered_source g_sources[NDI_FINDER_MAX_SOURCES];
static int g_source_count = 0;

static void append_ip_token(char *dst, size_t dst_size, const char *token)
{
	if (!dst || !dst_size || !token || !token[0])
		return;
	const size_t tok_len = strlen(token);
	/* Exact comma-delimited deduplication (avoid substring false positives like 10.0.0.1 vs 10.0.0.10). */
	const char *p = dst;
	while (*p) {
		const char *comma = strchr(p, ',');
		const size_t seg_len = comma ? (size_t)(comma - p) : strlen(p);
		if (seg_len == tok_len && memcmp(p, token, tok_len) == 0)
			return;
		if (!comma)
			break;
		p = comma + 1;
	}
	const size_t cur = strlen(dst);
	if (cur + tok_len + 2 >= dst_size)
		return;
	if (cur > 0)
		dst[cur] = ',';
	snprintf(dst + (cur > 0 ? cur + 1 : 0), dst_size - (cur > 0 ? cur + 1 : 0), "%s", token);
}

static void rebuild_merged_user_ips_locked(void)
{
	char merged[sizeof(g_user_extra_ips)];
	merged[0] = '\0';
	for (int i = 0; i < NDI_FINDER_MAX_OWNERS; i++) {
		if (!g_owner_ips[i].in_use || !g_owner_ips[i].ips[0])
			continue;
		char tmp[256];
		snprintf(tmp, sizeof(tmp), "%s", g_owner_ips[i].ips);
		char *save = NULL;
		for (char *tok = strtok_r(tmp, ",; \t\r\n", &save); tok; tok = strtok_r(NULL, ",; \t\r\n", &save))
			append_ip_token(merged, sizeof(merged), tok);
	}
	if (strcmp(g_user_extra_ips, merged) != 0) {
		snprintf(g_user_extra_ips, sizeof(g_user_extra_ips), "%s", merged);
		g_finder_recreate = true;
		pthread_cond_broadcast(&g_finder_cond);
	}
}

static void build_combined_extra_ips(char *out, size_t out_size, const char *user_ips,
				     struct ndi_discovered_source *nsd_out, int *nsd_count, int nsd_max)
{
	out[0] = '\0';
	if (nsd_count)
		*nsd_count = 0;

	if (user_ips && user_ips[0]) {
		char tmp[1024];
		snprintf(tmp, sizeof(tmp), "%s", user_ips);
		char *save = NULL;
		for (char *tok = strtok_r(tmp, ",; \t\r\n", &save); tok; tok = strtok_r(NULL, ",; \t\r\n", &save))
			append_ip_token(out, out_size, tok);
	}

#if defined(__ANDROID__)
	struct obs_android_ndi_peer peers[32];
	const int n = obs_android_ndi_enum_peers(peers, 32);
	for (int i = 0; i < n; i++) {
		if (peers[i].address[0])
			append_ip_token(out, out_size, peers[i].address);
		if (nsd_out && nsd_count && *nsd_count < nsd_max) {
			struct ndi_discovered_source *dst = &nsd_out[*nsd_count];
			snprintf(dst->name, sizeof(dst->name), "%s", peers[i].name);
			snprintf(dst->url, sizeof(dst->url), "%s", peers[i].address);
			dst->from_sdk = false;
			(*nsd_count)++;
		}
	}
#else
	(void)nsd_out;
	(void)nsd_count;
	(void)nsd_max;
#endif
}

static void *finder_thread_main(void *arg)
{
	(void)arg;
	const struct ndi_api *api = ndi_runtime_acquire();
	NDIlib_find_instance_t finder = NULL;
	char active_ips[1024] = {0};

	for (;;) {
		pthread_mutex_lock(&g_finder_mtx);
		if (g_finder_stop) {
			pthread_mutex_unlock(&g_finder_mtx);
			break;
		}
		char user_ips[1024];
		snprintf(user_ips, sizeof(user_ips), "%s", g_user_extra_ips);
		bool need_recreate = g_finder_recreate;
		g_finder_recreate = false;
		pthread_mutex_unlock(&g_finder_mtx);

		if (!api)
			api = ndi_runtime_acquire();

		struct ndi_discovered_source merged[NDI_FINDER_MAX_SOURCES];
		int merged_count = 0;
		char combined_ips[1024] = {0};
		build_combined_extra_ips(combined_ips, sizeof(combined_ips), user_ips, merged, &merged_count,
					 NDI_FINDER_MAX_SOURCES);

		if (api) {
			if (!finder || need_recreate || strcmp(combined_ips, active_ips) != 0) {
				if (finder) {
					api->find_destroy(finder);
					finder = NULL;
				}
				NDIlib_find_create_t cfg;
				memset(&cfg, 0, sizeof(cfg));
				cfg.show_local_sources = true;
				cfg.p_groups = NULL;
				cfg.p_extra_ips = combined_ips[0] ? combined_ips : NULL;
				finder = api->find_create_v2(&cfg);
				snprintf(active_ips, sizeof(active_ips), "%s", combined_ips);
			}

			if (finder) {
				api->find_wait_for_sources(finder, 400);
				uint32_t sdk_count = 0;
				const NDIlib_source_t *sdk_srcs = api->find_get_current_sources(finder, &sdk_count);
				if (sdk_srcs && sdk_count > 0) {
					/* Prefer SDK-resolved canonical NDI names over raw NsdManager entries. */
					merged_count = 0;
					for (uint32_t i = 0; i < sdk_count && merged_count < NDI_FINDER_MAX_SOURCES; i++) {
						if (!sdk_srcs[i].p_ndi_name || !sdk_srcs[i].p_ndi_name[0])
							continue;
						struct ndi_discovered_source *dst = &merged[merged_count++];
						snprintf(dst->name, sizeof(dst->name), "%s", sdk_srcs[i].p_ndi_name);
						snprintf(dst->url, sizeof(dst->url), "%s",
							 sdk_srcs[i].p_url_address ? sdk_srcs[i].p_url_address : "");
						dst->from_sdk = true;
					}
				}
			}
		}

		pthread_mutex_lock(&g_finder_mtx);
		g_source_count = merged_count;
		if (merged_count > 0)
			memcpy(g_sources, merged, (size_t)merged_count * sizeof(g_sources[0]));
		if (!g_finder_stop && !g_finder_recreate) {
			struct timespec ts;
			clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_nsec += 500000000L; /* 500 ms */
			if (ts.tv_nsec >= 1000000000L) {
				ts.tv_sec += 1;
				ts.tv_nsec -= 1000000000L;
			}
			pthread_cond_timedwait(&g_finder_cond, &g_finder_mtx, &ts);
		}
		pthread_mutex_unlock(&g_finder_mtx);
	}

	if (api && finder)
		api->find_destroy(finder);
	if (api)
		ndi_runtime_release();
	return NULL;
}

void ndi_finder_acquire(void)
{
	pthread_mutex_lock(&g_finder_mtx);
	/* Wait if a previous release() is currently joining the exiting finder thread. */
	while (g_finder_joining)
		pthread_cond_wait(&g_finder_state_cond, &g_finder_mtx);

	g_finder_refs++;
	if (!g_finder_running) {
		g_finder_stop = false;
		g_finder_recreate = true;
#if defined(__ANDROID__)
		obs_android_ndi_acquire_discovery();
#endif
		if (pthread_create(&g_finder_thread, NULL, finder_thread_main, NULL) == 0) {
			g_finder_running = true;
		} else {
#if defined(__ANDROID__)
			obs_android_ndi_release_discovery();
#endif
		}
	}
	pthread_mutex_unlock(&g_finder_mtx);
}

void ndi_finder_release(void)
{
	pthread_mutex_lock(&g_finder_mtx);
	while (g_finder_joining)
		pthread_cond_wait(&g_finder_state_cond, &g_finder_mtx);

	if (g_finder_refs > 0)
		g_finder_refs--;
	const bool should_stop = (g_finder_refs == 0 && g_finder_running);
	pthread_t thread_to_join;
	if (should_stop) {
		g_finder_stop = true;
		g_finder_joining = true;
		thread_to_join = g_finder_thread;
		pthread_cond_broadcast(&g_finder_cond);
	}
	pthread_mutex_unlock(&g_finder_mtx);

	if (should_stop) {
		pthread_join(thread_to_join, NULL);
#if defined(__ANDROID__)
		obs_android_ndi_release_discovery();
#endif
		pthread_mutex_lock(&g_finder_mtx);
		g_finder_running = false;
		g_finder_joining = false;
		g_source_count = 0;
		pthread_cond_broadcast(&g_finder_state_cond);
		pthread_mutex_unlock(&g_finder_mtx);
	}
}

void ndi_finder_set_source_extra_ips(const void *owner_key, const char *extra_ips)
{
	pthread_mutex_lock(&g_finder_mtx);
	const char *val = extra_ips ? extra_ips : "";
	int slot = -1;
	int free_slot = -1;
	for (int i = 0; i < NDI_FINDER_MAX_OWNERS; i++) {
		if (g_owner_ips[i].in_use && g_owner_ips[i].owner == owner_key) {
			slot = i;
			break;
		}
		if (!g_owner_ips[i].in_use && free_slot < 0)
			free_slot = i;
	}
	if (!val[0]) {
		if (slot >= 0) {
			g_owner_ips[slot].in_use = false;
			g_owner_ips[slot].owner = NULL;
			g_owner_ips[slot].ips[0] = '\0';
		}
	} else {
		if (slot < 0)
			slot = free_slot;
		if (slot >= 0) {
			g_owner_ips[slot].in_use = true;
			g_owner_ips[slot].owner = owner_key;
			snprintf(g_owner_ips[slot].ips, sizeof(g_owner_ips[slot].ips), "%s", val);
		}
	}
	rebuild_merged_user_ips_locked();
	pthread_mutex_unlock(&g_finder_mtx);
}

void ndi_finder_remove_source_extra_ips(const void *owner_key)
{
	ndi_finder_set_source_extra_ips(owner_key, "");
}

void ndi_finder_set_extra_ips(const char *extra_ips)
{
	ndi_finder_set_source_extra_ips(NULL, extra_ips);
}

void ndi_finder_refresh(void)
{
	pthread_mutex_lock(&g_finder_mtx);
	g_finder_recreate = true;
	pthread_cond_broadcast(&g_finder_cond);
	pthread_mutex_unlock(&g_finder_mtx);
}

int ndi_finder_get_sources(struct ndi_discovered_source *out, int max)
{
	if (!out || max <= 0)
		return 0;
	pthread_mutex_lock(&g_finder_mtx);
	const int n = g_source_count < max ? g_source_count : max;
	if (n > 0)
		memcpy(out, g_sources, (size_t)n * sizeof(out[0]));
	pthread_mutex_unlock(&g_finder_mtx);
	return n;
}
